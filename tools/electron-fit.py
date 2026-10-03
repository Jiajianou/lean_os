#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "build", "chromium", "src")
LEAN_OS_PORT = os.path.join(ROOT, "tools", "chromium-port")
ELECTRON_PORT = os.path.join(ROOT, "tools", "electron-port")


def git(repo, *args, env=None, check=True, data=None):
    result = subprocess.run(["git", "-C", repo] + list(args), input=data,
                            capture_output=True, env=env)
    if check and result.returncode != 0:
        raise RuntimeError("git %s in %s: %s" % (" ".join(args), repo,
                                                  result.stderr.decode()))
    return result


class Repository:
    # A temporary index built from the repository's own HEAD is a working
    # tree nobody can see: git apply --cached writes the result there and
    # nowhere else, so measuring how three patch series fit a checkout leaves
    # the checkout - and the browser being built out of it - untouched.
    def __init__(self, src, relative, scratch):
        self.relative = relative
        self.path = os.path.join(src, relative) if relative else src
        name = relative.replace("/", "_") or "src"
        self.index = os.path.join(scratch, name + ".index")
        self.env = dict(os.environ, GIT_INDEX_FILE=self.index)
        git(self.path, "read-tree", "HEAD", env=self.env)

    def apply(self, text, extra=()):
        result = git(self.path, "apply", "--cached", "-p1", *extra, "-",
                     env=self.env, check=False, data=text)
        return result.returncode == 0, result.stderr.decode()


def is_repository_root(src, relative):
    path = os.path.join(src, relative)
    if not os.path.isdir(path):
        return False
    top = git(path, "rev-parse", "--show-toplevel", check=False)
    return top.returncode == 0 and \
        os.path.realpath(top.stdout.decode().strip()) == os.path.realpath(path)


def sections(text):
    # A patch is a run of per-file sections, and this series writes them two
    # ways: git's, which open with "diff --git", and plain unified diffs, which
    # open with a "---"/"+++" pair and nothing before it - seven of this fork's
    # patches are only the second kind and two are both. Splitting on the git
    # line alone saw no file at all in those seven and counted them applied.
    # A "---" line inside a hunk is a removed line that begins "--", so the
    # hunks' own line counts say where a header can be.
    lines = text.split(b"\n")
    starts = []
    old = new = 0
    in_git_header = False
    for i, line in enumerate(lines):
        if old > 0 or new > 0:
            if line.startswith(b"-"):
                old -= 1
            elif line.startswith(b"+"):
                new -= 1
            elif line.startswith(b"\\"):
                pass
            else:
                old -= 1
                new -= 1
            continue
        if line.startswith(b"diff --git "):
            starts.append(i)
            in_git_header = True
            continue
        if line.startswith(b"--- ") and i + 1 < len(lines) and \
                lines[i + 1].startswith(b"+++ "):
            if not in_git_header:
                starts.append(i)
            in_git_header = False
            continue
        match = re.match(rb"@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@", line)
        if match:
            old = int(match.group(1)) if match.group(1) is not None else 1
            new = int(match.group(2)) if match.group(2) is not None else 1
            in_git_header = False
    if not starts:
        return text, []
    header = b"\n".join(lines[:starts[0]]) + (b"\n" if starts[0] else b"")
    parts = []
    for n, start in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else len(lines)
        chunk = b"\n".join(lines[start:end])
        if end < len(lines):
            chunk += b"\n"
        parts.append(chunk)
    return header, parts


def section_path(section):
    match = re.match(rb"diff --git a/(\S+) b/", section)
    if match:
        return match.group(1).decode()
    match = re.search(rb"(?m)^--- a/(\S+)", section) or \
        re.search(rb"(?m)^\+\+\+ b/(\S+)", section)
    return match.group(1).decode()


def owning_repository(src, path, cache):
    directory = os.path.dirname(os.path.join(src, path))
    while not os.path.isdir(directory):
        directory = os.path.dirname(directory)
    if directory not in cache:
        top = git(directory, "rev-parse", "--show-toplevel"
                  ).stdout.decode().strip()
        relative = os.path.relpath(os.path.realpath(top),
                                   os.path.realpath(src))
        cache[directory] = "" if relative == "." else relative
    return cache[directory]


def strip_prefix(section, prefix):
    if not prefix:
        return section
    old = prefix.encode() + b"/"
    lines = section.split(b"\n")
    for i, line in enumerate(lines):
        if line.startswith(b"@@"):
            break
        for marker in (b"a/", b"b/"):
            line = line.replace(b" " + marker + old, b" " + marker)
        lines[i] = line
    return b"\n".join(lines)


def load_fits(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return json.load(f)


def fit(text, series, name, fits, used):
    # Electron's series is somebody else's and is applied as they wrote it.
    # Where it meets this checkout's revision or this fork's own series, the
    # difference is an anchored edit to the patch text with its reason beside
    # it - so what was changed is a list rather than a second copy of a
    # four-thousand-line patch that would drift from the first.
    for number, rule in enumerate(fits):
        if rule["series"] != series or rule["patch"] != name:
            continue
        if "drop_file" in rule:
            header, parts = sections(text)
            kept = [p for p in parts if section_path(p) != rule["drop_file"]]
            if len(kept) == len(parts):
                raise RuntimeError("fit %d: %s has no section for %s" % (
                    number, name, rule["drop_file"]))
            text = header + b"".join(kept)
        else:
            find = "\n".join(rule["find"]).encode() + b"\n"
            replace = "\n".join(rule["replace"]).encode() + b"\n"
            if text.count(find) != 1:
                raise RuntimeError("fit %d: anchor found %d times in %s" % (
                    number, text.count(find), name))
            text = text.replace(find, replace)
        used.add(number)
    return text


def electron_series(electron):
    with open(os.path.join(electron, "patches", "config.json")) as f:
        config = json.load(f)
    for entry in config:
        directory = os.path.join(os.path.dirname(electron),
                                 entry["patch_dir"][len("src/"):])
        order = os.path.join(directory, ".patches")
        names = []
        if os.path.exists(order):
            with open(order) as f:
                names = [n.strip() for n in f if n.strip()]
        repo = entry["repo"]
        relative = "" if repo == "src" else repo[len("src/"):]
        yield (os.path.basename(directory), relative, directory, names)


def own_series(directory):
    if not os.path.isdir(directory):
        return []
    return [os.path.join(directory, n) for n in sorted(os.listdir(directory))
            if n.endswith(".patch")]


def measure(src, electron, fits_path, emit=None, verbose=False, only=None,
            lean_os_port=LEAN_OS_PORT):
    fits = load_fits(fits_path)
    used = set()
    report = {"lean_os": {"applied": 0, "failed": []},
              "electron": {},
              "electron_port": {"applied": 0, "failed": []},
              "missing_repositories": [], "unused_fits": []}
    with tempfile.TemporaryDirectory() as scratch:
        repos = {}
        cache = {}

        def repository(relative):
            if relative not in repos:
                repos[relative] = Repository(src, relative, scratch)
            return repos[relative]

        def apply_own(paths, key):
            for path in paths:
                with open(path, "rb") as f:
                    _, parts = sections(f.read())
                if not parts:
                    report[key]["failed"].append(os.path.basename(path) +
                                                 " (no file in it)")
                    continue
                groups = {}
                for section in parts:
                    relative = owning_repository(src, section_path(section),
                                                 cache)
                    if only is not None and relative != only:
                        continue
                    groups.setdefault(relative, []).append(
                        strip_prefix(section, relative))
                ok = True
                for relative, chunk in groups.items():
                    applied, error = repository(relative).apply(b"".join(chunk))
                    if not applied:
                        ok = False
                        if verbose:
                            print("%s %s [%s]: %s" % (
                                key, os.path.basename(path), relative or "src",
                                error.strip()))
                if ok:
                    report[key]["applied"] += 1
                else:
                    report[key]["failed"].append(os.path.basename(path))

        apply_own(own_series(lean_os_port), "lean_os")

        order = []
        for series, relative, directory, names in electron_series(electron):
            key = relative or "src"
            if only is not None and relative != only:
                continue
            entry = {"total": len(names), "applied": 0, "reduced_context": [],
                     "failed": {}}
            report["electron"][key] = entry
            if not names:
                continue
            if not is_repository_root(src, relative):
                report["missing_repositories"].append(key)
                entry["missing"] = True
                continue
            repo = repository(relative)
            for name in names:
                with open(os.path.join(directory, name), "rb") as f:
                    text = fit(f.read(), series, name, fits, used)
                if emit:
                    out = os.path.join(emit, series)
                    os.makedirs(out, exist_ok=True)
                    with open(os.path.join(out, name), "wb") as f:
                        f.write(text)
                if not sections(text)[1]:
                    entry["applied"] += 1
                    entry.setdefault("emptied", []).append(name)
                    continue
                applied, error = repo.apply(text)
                if applied:
                    entry["applied"] += 1
                    continue
                if verbose:
                    print("electron %s [%s]: %s" % (name, key, error.strip()))
                applied, _ = repo.apply(text, ("-C1",))
                if applied:
                    entry["reduced_context"].append(name)
                    continue
                files = sorted(set(re.findall(
                    r"error: (?:patch failed: )?([^:\s]+)", error)))
                entry["failed"][name] = files
            if emit:
                order.append(series)
                with open(os.path.join(emit, series, ".patches"), "w") as f:
                    f.write("".join(n + "\n" for n in names))
                with open(os.path.join(emit, series, ".repository"), "w") as f:
                    f.write(key + "\n")

        apply_own(own_series(ELECTRON_PORT), "electron_port")
        if emit:
            with open(os.path.join(emit, ".series"), "w") as f:
                f.write("".join(s + "\n" for s in order))
            for path in own_series(ELECTRON_PORT):
                out = os.path.join(emit, "lean_os")
                os.makedirs(out, exist_ok=True)
                shutil.copy(path, out)

    report["unused_fits"] = [n for n in range(len(fits)) if n not in used]
    return report


def failures(report):
    found = []
    found += ["lean_os " + n for n in report["lean_os"]["failed"]]
    for key, entry in report["electron"].items():
        if entry.get("missing"):
            found.append("electron %s not fetched" % key)
        found += ["electron %s" % n for n in entry["failed"]]
        found += ["electron %s needs fuzz" % n for n in entry["reduced_context"]]
    found += ["electron-port " + n for n in report["electron_port"]["failed"]]
    found += ["fit %d matched no patch" % n for n in report["unused_fits"]]
    return found


def show(report):
    lean = report["lean_os"]
    print("lean_os series:        %d applied, %d failed" % (
        lean["applied"], len(lean["failed"])))
    for key, entry in report["electron"].items():
        if not entry["total"]:
            continue
        if entry.get("missing"):
            print("electron %-28s not fetched (%d patches)" % (key, entry["total"]))
            continue
        print("electron %-28s %3d/%3d exact, %d need fuzz, %d failed" % (
            key, entry["applied"], entry["total"],
            len(entry["reduced_context"]), len(entry["failed"])))
    own = report["electron_port"]
    print("electron-port series:  %d applied, %d failed" % (
        own["applied"], len(own["failed"])))
    for line in failures(report):
        print("  FAIL " + line)


def self_test(src, electron):
    # The instrument has to be able to fail. A fit whose anchor has drifted,
    # a patch that needs fuzz, and a repository that is not there each have to
    # come back as a failure, or a clean report means nothing.
    problems = []
    with tempfile.TemporaryDirectory() as scratch:
        fits = load_fits(os.path.join(ELECTRON_PORT, "fits.json"))
        broken = [dict(rule) for rule in fits]
        anchored = next(i for i, r in enumerate(broken) if "find" in r)
        broken[anchored]["find"] = ["this anchor is in no patch"]
        path = os.path.join(scratch, "fits.json")
        with open(path, "w") as f:
            json.dump(broken, f)
        try:
            measure(src, electron, path, only="")
            problems.append("a fit with a drifted anchor was not refused")
        except RuntimeError:
            pass

        unfitted = [r for r in fits if "find" not in r or
                    r["patch"] != "feat_corner_smoothing_css_rule_and_blink_painting.patch"]
        with open(path, "w") as f:
            json.dump(unfitted, f)
        report = measure(src, electron, path, only="")
        if not any("needs fuzz" in line for line in failures(report)):
            problems.append("a patch that only applies with fuzz was not reported")

        spare = fits + [{"series": "chromium", "patch": "no-such.patch",
                         "find": ["x"], "replace": ["y"]}]
        with open(path, "w") as f:
            json.dump(spare, f)
        report = measure(src, electron, path, only="")
        if not any("matched no patch" in line for line in failures(report)):
            problems.append("a fit that matched nothing was not reported")
        port = os.path.join(scratch, "port")
        shutil.copytree(LEAN_OS_PORT, port)
        with open(os.path.join(port, "9999-plain.patch"), "w") as f:
            f.write("--- a/base/BUILD.gn\n+++ b/base/BUILD.gn\n"
                    "@@ -1,1 +1,1 @@\n-this line is in no file\n+nor is this\n")
        report = measure(src, electron, os.path.join(ELECTRON_PORT, "fits.json"),
                         only="", lean_os_port=port)
        if "9999-plain.patch" not in report["lean_os"]["failed"]:
            problems.append("a plain diff that cannot apply was counted applied")
    for problem in problems:
        print("electron-fit self-test: " + problem)
    if not problems:
        print("electron-fit self-test: 4 broken inputs, 4 refused")
    return 1 if problems else 0


def changed_paths(text):
    found = []
    for section in sections(text)[1]:
        old = re.search(rb"(?m)^--- (\S+)", section)
        new = re.search(rb"(?m)^\+\+\+ (\S+)", section)
        if old and old.group(1) == b"/dev/null" and new:
            found.append(("A", new.group(1).decode()[2:]))
        else:
            found.append(("M", section_path(section)))
    return found


def every_path(fitted):
    # Every file any of the three series modifies or creates, relative to src,
    # so a build can put each one back before applying the series it wants.
    # A browser build has to reset what ELECTRON touched as well as what this
    # fork touched, or the next /bin/chrome is built from Electron's edits.
    found = set()
    for path in own_series(LEAN_OS_PORT) + own_series(ELECTRON_PORT):
        with open(path, "rb") as f:
            found.update(changed_paths(f.read()))
    order = os.path.join(fitted, ".series")
    if os.path.exists(order):
        with open(order) as f:
            series = [s.strip() for s in f if s.strip()]
        for name in series:
            directory = os.path.join(fitted, name)
            with open(os.path.join(directory, ".repository")) as f:
                repository = f.read().strip()
            prefix = "" if repository == "src" else repository + "/"
            with open(os.path.join(directory, ".patches")) as f:
                names = [n.strip() for n in f if n.strip()]
            for patch in names:
                with open(os.path.join(directory, patch), "rb") as f:
                    for kind, rel in changed_paths(f.read()):
                        found.add((kind, prefix + rel))
    # A file one patch creates and a later one edits is still a file no
    # repository has: it is put back by removing it, never by a checkout,
    # which would fail on it quietly and leave the next apply to stop on
    # "already exists".
    created = {rel for kind, rel in found if kind == "A"}
    return sorted((kind, rel) for kind, rel in found
                  if kind == "A" or rel not in created)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", default=SRC)
    parser.add_argument("--electron", default=None)
    parser.add_argument("--fits", default=os.path.join(ELECTRON_PORT, "fits.json"))
    parser.add_argument("--json", default=None)
    parser.add_argument("--emit", default=None)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--paths", default=None, metavar="FITTED")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    electron = args.electron or os.path.join(args.src, "electron")
    if args.self_test:
        return self_test(args.src, electron)
    if args.paths:
        for kind, rel in every_path(args.paths):
            print(kind, rel)
        return 0
    if args.emit and os.path.exists(args.emit):
        shutil.rmtree(args.emit)
    report = measure(args.src, electron, args.fits, args.emit, args.verbose)
    show(report)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=1, sort_keys=True)
    return 1 if failures(report) else 0


if __name__ == "__main__":
    sys.exit(main())
