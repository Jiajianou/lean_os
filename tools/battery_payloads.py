#!/usr/bin/env python3
"""Which of the graded battery's markers may be SKIPPED, and why.

tools/qemu-serial-test.sh requires every marker in its REQUIRED_MARKERS. About
sixty of them grade a payload that no `make` target builds - the cross gcc and
clang ports, Python, the package repository, Rust, Chromium's //base to
content_shell, Node - and a host that has not built those (an Intel Mac with
no Chromium checkout, a fresh arm64 one) can never see them. The guest already
says so: each self-test whose program is not on the image logs "... is not on
this image - skipped." and moves on. Until this file, the battery could not
tell that from a regression, so it could not go green on such a host at all.

The rule is the interactive suite's (tools/qemu_input_suite.py,
payload_decisions), applied to markers instead of tests. A required marker
that never appeared is SKIPPED only when all of these hold:

  1. it belongs to a payload in PAYLOADS below - any other marker is required,
     always;
  2. the payload is not on the image the guest booted, read on this side by
     the same reader the suite uses (qemu_input.ImageFiles over
     tools/leanfs-fsck.py), never guessed from build/;
  3. this host has NOT built it: the paths its installer installs FROM are
     absent. Built here and missing from the image is M113's failure - a
     `make all` recreated the disk after the payload went on - and FAILS,
     a child payload (one the guest runs only under its parent) included,
     whatever its parent's state;
  4. the guest said it skipped, in its own words (`skip` below), so a hang or a
     self-test that silently stopped running cannot pass for an absence.

A panic is a FAIL whatever was skipped (the shell harness checks that first),
an image this reader cannot read justifies no skip at all, and a payload whose
program lives in this tree (toybox, /bin/mathltest) is "built here" on every
host - a missing [m142q] means "run tools/math-long-double-test.sh", not
"skipped".

    battery_payloads.py probe --image IMG --state FILE   before the boot
    battery_payloads.py grade --state FILE --log LOG --total N [--summary F]
                                                         < missing markers
    battery_payloads.py --self-test

LEANOS_PAYLOAD_HOST_ROOT moves where "built here" is looked for (default: this
checkout), for the battery and for the input suite's browser tests alike
(BROWSER_BUILT and built_paths are the one rule both use). It exists for the
self-test and for showing the M113 failure on a host that has built nothing;
nothing else should set it.
"""

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
if HERE not in sys.path:
    sys.path.insert(0, HERE)
# Run as a script, this module is __main__; the input suite imports it as
# battery_payloads. One module, so the suite reads this very table.
if __name__ == "__main__":
    sys.modules.setdefault("battery_payloads", sys.modules[__name__])

# The self-test's end-to-end checks run tools/qemu-serial-test.sh in its
# grade-only mode, which takes a few seconds. The ceiling it is handed is
# what a boot would get if that mode ever stopped working (instead of the
# default 1500 s); the timeout is when the self-test stops waiting at all.
GRADE_ONLY_CEILING = 5
GRADE_ONLY_TIMEOUT = 120


# tools/chromium-test.sh and tools/install-browser.sh both install Chromium's
# programs FROM build/chromium/src/out/$LEANOS_CHROMIUM_OUT (LeanOS when it is
# unset). The tables below hold a placeholder for it, resolved when "built
# here" is asked (built_paths), so the variable is read when a decision is
# taken - not when this module happened to be imported.
_OUT_DIR = "{LEANOS_CHROMIUM_OUT}"


def _chromium_out(name):
    return "build/chromium/src/out/%s/%s" % (_OUT_DIR, name)


def resolve_built(path):
    """A `built` path where this host's installers would look for it now."""
    return path.replace(_OUT_DIR, os.environ.get("LEANOS_CHROMIUM_OUT") or "LeanOS")


# What tools/install-browser.sh (make browser-if-built) installs from - the ONE
# table both the graded battery (PAYLOADS below) and the interactive suite
# (tools/qemu_input_suite.py's PAYLOADS) read "this host has built the browser"
# from. Groups, as everywhere here: each group needs one of its paths.
# install-browser.sh installs nothing without content_shell, //chrome included,
# so a host that linked only //chrome has not built a browser it could
# install, and the image without one is a skip there, not M113.
BROWSER_BUILT = {
    "content_shell": [(_chromium_out("content_shell"),)],
    "chrome": [(_chromium_out("content_shell"),), (_chromium_out("chrome"),)],
}


# A program in this tree, built by a script that needs nothing `make all` does
# not already need. Never "not built here".
ALWAYS = "always"

_CHROMIUM_HOW = "tools/build-chromium.sh lean_os links it and tools/chromium-test.sh installs it"


def _chromium(name, program, built, mark, markers):
    return dict(name=name, image=[("/bin/" + program,)],
                built=BROWSER_BUILT.get(built) or [(_chromium_out(built),)],
                skip="[%s] /bin/%s is not on this image - skipped." % (mark, program),
                how=_CHROMIUM_HOW, markers=markers)


# Each payload: the image paths the kernel's self-test stats before running it
# (groups; any one path of a group will do), the host paths its installer
# installs FROM (groups again; all groups present = built here), the line the
# guest logs when it skips, and the markers only it produces. `parent` is a
# payload whose absence makes the guest skip this one without a word of its
# own (the [m97] halves under /bin/cxxtest): with the parent absent, the
# parent decides - unless this host built the child and the image lost it,
# which is M113 on the child's own account whatever the parent's verdict.
PAYLOADS = [
    # make toybox (build/toybox/toybox, from third_party/toybox).
    dict(name="toybox", image=[("/bin/toybox",)], built=ALWAYS,
         skip="[m89] /bin/toybox is not on this image - skipped.",
         how="`make toybox` builds it from third_party/toybox and installs it",
         markers=["[m89] somebody else's userland: find, xargs, grep, sort and uniq"]),
    # tools/math-long-double-test.sh: make build/mathltest.elf, then leanfs-put.
    dict(name="mathltest", image=[("/bin/mathltest",)], built=ALWAYS,
         skip="[m142] /bin/mathltest is not on this image - skipped.",
         how="tools/math-long-double-test.sh builds and installs it",
         markers=["[m142] a long double library for the x87's own format:",
                  "[m142q] a quiet NaN through every function <math.h> declares:",
                  "mathltest: done"]),
    # tools/gcc-test.sh: build/gcctest, compiled by build/toolchain's gcc.
    dict(name="gcctest", image=[("/bin/gcctest",)], built=[("build/gcctest",)],
         skip="[m94] /bin/gcctest is not on this image - skipped.",
         how="tools/build-toolchain.sh, then tools/gcc-test.sh",
         markers=["[m94] a target this compiler knows by name"]),
    # tools/gcc-test.sh again, from build/thirdparty/ (tools/build-thirdparty.sh).
    dict(name="bzip2+gnuhello", parent="gcctest",
         image=[("/bin/bzip2",), ("/bin/gnuhello",)],
         built=[("build/thirdparty/bzip2",), ("build/thirdparty/gnuhello",)],
         skip="[m94] no ported third-party programs on this image",
         how="tools/build-thirdparty.sh, then tools/gcc-test.sh",
         markers=["[m94] somebody else's project: bzip2, built with"]),
    dict(name="zlib", image=[("/bin/zlibtest",)], built=[("build/thirdparty/zlibtest",)],
         skip="[m100] /bin/zlibtest is not on this image - skipped.",
         how="tools/build-thirdparty.sh, then tools/gcc-test.sh",
         markers=["[m100] the first library of the stack:"]),
    dict(name="libpng+libjpeg", image=[("/bin/djpeg",)], built=[("build/thirdparty/djpeg",)],
         skip="[m100b] /bin/djpeg is not on this image - skipped.",
         how="tools/build-thirdparty.sh, then tools/gcc-test.sh",
         markers=["[m100b] two more libraries, graded by their own suites:"]),
    dict(name="freetype+expat", image=[("/bin/ftrender",)], built=[("build/thirdparty/ftrender",)],
         skip="[m100c] /bin/ftrender is not on this image - skipped.",
         how="tools/build-thirdparty.sh, then tools/gcc-test.sh",
         markers=["[m100c] freetype against the host, and expat by its own suite:"]),
    dict(name="sqlite", image=[("/bin/sqlite3",)], built=[("build/thirdparty/sqlite3",)],
         skip="[m100d] /bin/sqlite3 is not on this image - skipped.",
         how="tools/build-thirdparty.sh, then tools/gcc-test.sh",
         markers=["[m100d] sqlite against the host:"]),
    dict(name="harfbuzz", image=[("/bin/hbshape",)], built=[("build/thirdparty/hbshape",)],
         skip="[m100e] /bin/hbshape is not on this image - skipped.",
         how="tools/build-thirdparty.sh, then tools/gcc-test.sh",
         markers=["[m100e] harfbuzz against the host:"]),
    dict(name="mbedtls", image=[("/bin/ssl_server2",)], built=[("build/thirdparty/ssl_server2",)],
         skip="[m100f] /bin/ssl_server2 is not on this image - skipped.",
         how="tools/build-thirdparty.sh, then tools/gcc-test.sh",
         markers=["[m100f] TLS end to end over M66's TCP:"]),
    dict(name="mbedtls-suites", image=[("/usr/share/m100/mbedtls/test_suite_shax",)],
         built=[("build/thirdparty/mbedtls-suites/test_suite_shax",)],
         skip="[m100g] mbedtls's suites are not on this image - skipped.",
         how="tools/build-thirdparty.sh, then tools/gcc-test.sh",
         markers=["[m100g] mbedtls's own suites:"]),
    # tools/build-dynamic.sh: build/dynamic/.
    dict(name="dyntest", image=[("/bin/dyntest",)], built=[("build/dynamic/dyntest",)],
         skip="[m95] /bin/dyntest is not on this image - skipped.",
         how="tools/build-toolchain.sh, then tools/build-dynamic.sh",
         markers=["[m95] code that is loaded, not linked:", "shared library pages held:"]),
    dict(name="manydyn", image=[("/bin/manydyn",)], built=[("build/dynamic/manydyn",)],
         skip="[m99ld] /bin/manydyn is not on this image - skipped.",
         how="tools/build-toolchain.sh, then tools/build-dynamic.sh",
         markers=["[m99ld] a loader an interpreter can use:"]),
    # tools/cxx-test.sh: build/cxxtest, build/cxxlib, build/gnucxx0..7; and
    # tools/build-dynamic.sh: build/dynamic/throwmain.
    dict(name="cxxtest", image=[("/bin/cxxtest",)], built=[("build/cxxtest",)],
         skip="[m97] /bin/cxxtest is not on this image - skipped.",
         how="tools/build-toolchain.sh, then tools/cxx-test.sh",
         markers=["[m97] C++ that throws:"]),
    dict(name="cxxlib", parent="cxxtest", image=[("/bin/cxxlib",)], built=[("build/cxxlib",)],
         skip="[m97] /bin/cxxlib is not on this image - the standard-library half is skipped.",
         how="tools/build-toolchain.sh, then tools/cxx-test.sh",
         markers=["[m97] and the standard library on top of it:"]),
    dict(name="throwmain", parent="cxxtest", image=[("/bin/throwmain",)],
         built=[("build/dynamic/throwmain",)],
         skip="[m97] /bin/throwmain is not on this image - the shared-object half is skipped.",
         how="tools/build-toolchain.sh, then tools/build-dynamic.sh",
         markers=["[m97] and across a shared object:"]),
    dict(name="gnucxx", parent="cxxtest", image=[("/tests/gnucxx0",)], built=[("build/gnucxx0",)],
         skip="[m97] GCC's own libstdc++ tests are not on this image - that half is skipped.",
         how="tools/cxx-test.sh, from the GCC source tree tools/build-toolchain.sh unpacks",
         markers=["[m97] and C++ nobody here wrote:"]),
    # tools/install-native-toolchain.sh, from build/native/binutils.
    dict(name="native binutils", image=[("/bin/as",)],
         built=[("build/native/binutils/gas/as-new",)],
         skip="[m98] /bin/as is not on this image - skipped.",
         how="tools/build-native-toolchain.sh, then tools/install-native-toolchain.sh",
         markers=["[m98] binutils runs here:"]),
    # tools/install-python.sh, from build/python/build/python$(BUILDEXE).
    dict(name="python", image=[("/bin/python3",)],
         built=[("build/python/build/python", "build/python/build/python.exe")],
         skip="[m99] /bin/python3 is not on this image - skipped.",
         how="tools/build-python.sh, then tools/install-python.sh",
         markers=["[m99] somebody else's language runs here:"]),
    # make packages, from build/repo (tools/build-packages.sh).
    dict(name="packages", image=[("/pkg/repo/index",)], built=[("build/repo/index",)],
         skip="[m111] no package repository on this disk - skipped.",
         how="tools/build-packages.sh, then make packages",
         markers=["[m111] GNU grep 3.11, built here, installed by `os` and run",
                  "[m111] a package binary named `compositor` gets 0x0, not",
                  "[m111] the kernel's package registry:",
                  "[m111] a package manager: GNU grep 3.11, built here by"]),
    # tools/chromium-test.sh, from build/chromium/src/out/$LEANOS_CHROMIUM_OUT.
    _chromium("chromiumbase", "chromiumbase", "basetest", "m145",
              ["[m145] Chromium's //base links and runs on this machine:",
               "chromiumbase: done"]),
    _chromium("chromiummojo", "chromiummojo", "mojotest", "m148",
              ["[m148] Chromium's //mojo runs on this machine:",
               "[m149] two processes on one mojo connection:",
               "chromiummojo: done"]),
    _chromium("chromiumnet", "chromiumnet", "nettest", "m150",
              ["[m150] Chromium's //url and //net run on this machine:",
               "[m151] Chromium's //net opens a connection here:",
               "[m152] https on this machine:",
               "chromiumnet: done"]),
    _chromium("chromiumv8", "chromiumv8", "v8test", "m156",
              ["[m156] V8 runs on this machine:", "chromiumv8: done"]),
    _chromium("chromiumskia", "chromiumskia", "skiatest", "m157",
              ["[m157] Skia rasterises on this machine:", "chromiumskia: done"]),
    _chromium("chromiumcc", "chromiumcc", "ccpainttest", "m158",
              ["[m158] cc rasters on this machine:", "chromiumcc: done"]),
    _chromium("chromiumgpu", "chromiumgpu", "gpuinfotest", "m159",
              ["[m159] Chromium's own GPU configuration, on this machine:",
               "chromiumgpu: done"]),
    _chromium("chromiumcc2", "chromiumcc2", "cctest2", "m160",
              ["[m160] cc's layer path on this machine:", "chromiumcc2: done"]),
    _chromium("chromiumblink", "chromiumblink", "blinktest", "m161",
              ["[m161] Blink's platform layer on this machine:", "chromiumblink: done"]),
    _chromium("chromiumviz", "chromiumviz", "viztest", "m162",
              ["[m162] the display compositor on this machine:", "chromiumviz: done"]),
    _chromium("chromiumcontent", "chromiumcontent", "contenttest", "m165",
              ["[m165] //content on this machine:",
               "[m166] a renderer with strictly less authority:",
               "chromiumcontent: done"]),
    _chromium("content_shell", "chromiumshell", "content_shell", "m167",
              ["[m167] Chromium's own browser on this machine:", "chromiumshell: done"]),
    # make browser-if-built (tools/install-browser.sh) - the same content_shell,
    # graded by a different self-test with a skip line of its own.
    dict(name="browser", image=[("/bin/chromiumshell",)],
         built=BROWSER_BUILT["content_shell"],
         skip="[m113] /bin/chromiumshell is not on this image - skipped.",
         how="tools/build-chromium.sh content/shell:content_shell, then make browser",
         markers=["[m113] the browser is installed:"]),
    # tools/install-browser.sh installs //chrome only alongside content_shell.
    dict(name="chrome", image=[("/bin/chrome",)],
         built=BROWSER_BUILT["chrome"],
         skip="[m201] no /bin/chrome on this image to load - skipped.",
         how="tools/build-chromium.sh chrome, then make browser",
         markers=["[m201] a program's first read no longer holds up every other exec"]),
    # tools/node-test.sh, from Electron's own out directory.
    dict(name="node", image=[("/bin/node",)],
         built=[("build/chromium/src/out/ElectronNode/node",)],
         skip="[m223] /bin/node is not on this image - skipped.",
         how="tools/build-chromium.sh (Electron's configuration), then tools/node-test.sh",
         markers=["[m223] Node.js ran 8 checks, 0 failed"]),
    # tools/clang-test.sh: build/clangtest and build/mixedtest.
    dict(name="clangtest", image=[("/bin/clangtest",)], built=[("build/clangtest",)],
         skip="[m121] /bin/clangtest is not on this image - skipped.",
         how="tools/build-clang.sh, then tools/clang-test.sh",
         markers=["[m121] a second compiler that knows this OS by name:",
                  "ONE PROGRAM FROM TWO COMPILERS"]),
    # tools/rust-test.sh: build/rusttest and build/ruststd.
    dict(name="rusttest", image=[("/bin/rusttest",)], built=[("build/rusttest",)],
         skip="[m137] /bin/rusttest is not on this image - skipped.",
         how="tools/rust-test.sh (cargo and the cross toolchain)",
         markers=["[m137] a third language for this target:"]),
    dict(name="ruststd", image=[("/bin/ruststd",)], built=[("build/ruststd",)],
         skip="[m138] /bin/ruststd is not on this image - skipped.",
         how="tools/rust-test.sh (cargo and the cross toolchain)",
         markers=["[m138] the Rust standard library on this machine:", "[m138] ruststd: "]),
]

BY_NAME = {p["name"]: p for p in PAYLOADS}


def marker_owner(payloads=None):
    owner = {}
    for p in payloads or PAYLOADS:
        for m in p["markers"]:
            owner[m] = p
    return owner


def host_root():
    return os.environ.get("LEANOS_PAYLOAD_HOST_ROOT") or REPO_ROOT


def built_paths(groups, root=None):
    """The host paths this host would install a payload FROM, when every group
    has one; None when it has not built it. Paths are relative to the checkout
    (or to LEANOS_PAYLOAD_HOST_ROOT); an absolute one stands for itself.
    Existence is enough: a stale or broken artefact still says somebody built
    it here, and the conservative mistake is a FAIL rather than a skip. The
    battery and the interactive suite both decide "built here" with this."""
    root = root or host_root()
    found = []
    for group in groups:
        hit = [resolve_built(p) for p in group
               if os.path.exists(os.path.join(root, resolve_built(p)))]
        if not hit:
            return None
        found.append(hit[0])
    return ", ".join(found)


def built_here(payload, root=None):
    if payload["built"] == ALWAYS:
        return "this tree builds it on any host, so it is never skipped"
    return built_paths(payload["built"], root)


# --- the image, read before the boot ------------------------------------------

def probe(image):
    """{"image", "unreadable", "absent": {name: why}} for the image the guest
    is about to boot."""
    state = {"image": image, "unreadable": None, "absent": {}}
    try:
        import qemu_input
        files = qemu_input.ImageFiles(image)
        for p in PAYLOADS:
            missing = qemu_input.payload_missing(p["image"], files=files)
            if missing:
                state["absent"][p["name"]] = missing
    except (Exception, SystemExit) as exc:  # leanfs-fsck exits on some images
        state["unreadable"] = str(exc) or exc.__class__.__name__
    return state


# --- the decision ----------------------------------------------------------------

def decide(payload, state, log, root=None, _depth=0):
    """("skip", why) or ("fail", why) for a payload one of whose markers never
    appeared."""
    if not state or state.get("unreadable"):
        why = (state or {}).get("unreadable") or "no reading of the image was taken"
        return ("fail", "the image could not be read here (%s), so nothing "
                        "justifies a skip" % why)
    absent = state.get("absent", {})
    name = payload["name"]
    # Built here and not on the image is M113 whoever's block it runs in: a
    # child (/bin/throwmain, /bin/cxxlib, GCC's libstdc++ tests, bzip2 and
    # gnuhello) this host built is graded by its own build first, and only
    # then - not built here, or on the image - left to its absent parent.
    built = built_here(payload, root) if name in absent else None
    if built and payload["built"] != ALWAYS:
        return ("fail", "%s, and this host has built it (%s) - the image was "
                        "recreated after it was installed (M113); %s puts it back"
                % (absent[name], built, payload["how"]))
    parent = BY_NAME.get(payload.get("parent")) if _depth < 8 else None
    if parent and parent["name"] in absent:
        verdict, why = decide(parent, state, log, root, _depth + 1)
        return (verdict, "%s (under %s): %s" % (name, parent["name"], why))
    if name not in absent:
        return ("fail", "%s IS on the image, so its marker is required"
                % " and ".join(g[0] for g in payload["image"]))
    if payload["built"] == ALWAYS:
        return ("fail", "%s; %s - %s" % (absent[name], built, payload["how"]))
    if payload.get("skip") and payload["skip"] not in log:
        return ("fail", "%s and not built here, but the machine never said "
                        "\"%s\" - an absence it did not report is not a skip"
                % (absent[payload["name"]], payload["skip"]))
    return ("skip", "%s, not built on this host; it needs %s"
            % (absent[payload["name"]], payload["how"]))


def grade(missing, state, log, root=None):
    """(skipped, failed): skipped is [(payload, why, [markers])], failed is
    [(marker, why-or-None)] in the order the markers were given."""
    owner = marker_owner()
    skipped, failed, decided = [], [], {}
    for marker in missing:
        p = owner.get(marker)
        if p is None:
            failed.append((marker, None))
            continue
        if p["name"] not in decided:
            decided[p["name"]] = decide(p, state, log, root)
        verdict, why = decided[p["name"]]
        if verdict == "skip":
            for entry in skipped:
                if entry[0] is p:
                    entry[2].append(marker)
                    break
            else:
                skipped.append((p, why, [marker]))
        else:
            failed.append((marker, why))
    return skipped, failed


# --- the command line --------------------------------------------------------------

def _cmd_probe(image, state_path):
    state = probe(image)
    with open(state_path, "w") as f:
        json.dump(state, f)
    if state["unreadable"]:
        print("[harness] could not read %s's filesystem here (%s) - every payload "
              "marker is required this run" % (image, state["unreadable"]))
        return 0
    absent = [p for p in PAYLOADS if p["name"] in state["absent"]]
    if not absent:
        print("[harness] every optional payload the battery grades is on %s" % image)
        return 0
    lost = [p for p in absent if p["built"] != ALWAYS and built_here(p)]
    always = [p for p in absent if p["built"] == ALWAYS]
    print("[harness] %d of %d payloads the battery grades are not on %s:"
          % (len(absent), len(PAYLOADS), image))
    print("[harness]   not built on this host, may be skipped: %s"
          % (", ".join(p["name"] for p in absent if p not in lost and p not in always)
             or "none"))
    for p in always:
        print("WARNING: %s is not on the image, and %s - its markers will FAIL. "
              "%s." % (p["name"], built_here(p), p["how"]))
    for p in lost:
        print("WARNING: %s was built on this host (%s) and is not on the image - "
              "the image was recreated after it was installed (M113, and the "
              "chain in the M186 commit message). Its markers will FAIL; %s "
              "puts it back." % (p["name"], built_here(p), p["how"]))
    return 0


def _cmd_grade(state_path, log_path, total, summary_path):
    try:
        with open(state_path) as f:
            state = json.load(f)
    except (OSError, ValueError):
        state = None
    with open(log_path, "rb") as f:
        log = f.read().decode("utf-8", "replace")
    missing = [line.rstrip("\n") for line in sys.stdin if line.rstrip("\n")]
    skipped, failed = grade(missing, state, log)
    n_skipped = sum(len(m) for _p, _w, m in skipped)
    if skipped:
        print("SKIPPED: %d/%d required boot markers, for %d payload(s) that are not on "
              "this image, were never built on this host, and that the machine said it "
              "skipped - not run, and not counted as found:"
              % (n_skipped, total, len(skipped)))
        for p, why, markers in skipped:
            print("  %s - %s" % (p["name"], why))
            for m in markers:
                print("      - %s" % m)
    if failed:
        print("FAIL: %d/%d required boot markers never appeared (log capture ended "
              "too early, or a real regression - try a longer SECONDS first):"
              % (len(failed), total))
        for marker, why in failed:
            print("  - %s" % marker)
            if why:
                print("      %s" % why)
    if summary_path:
        with open(summary_path, "w") as f:
            f.write("%d %d\n" % (n_skipped, len(failed)))
    return 1 if failed else 0


# --- the self-test ------------------------------------------------------------------

def required_markers(script=None):
    """REQUIRED_MARKERS, read out of the shell harness that owns them."""
    text = open(script or os.path.join(HERE, "qemu-serial-test.sh")).read()
    body = text.split("REQUIRED_MARKERS=(", 1)[1].split("\n)\n", 1)[0]
    out = []
    for line in body.splitlines():
        line = line.strip()
        if line.startswith('"') and line.endswith('"'):
            out.append(line[1:-1].replace("\\`", "`").replace('\\"', '"').replace("\\$", "$"))
    return out


def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # The table against the two files it describes. A marker renamed in the
    # harness and not here would only ever FAIL (it is not ours to skip), but
    # one renamed here and not there would be dead weight that looks like
    # coverage; and a skip line the kernel no longer prints would turn every
    # skip of that payload into a failure on the next host without it.
    required = required_markers()
    check(len(required) > 150, "read only %d markers out of qemu-serial-test.sh" % len(required))
    # The kernel's lines are C string literals continued across source lines;
    # join adjacent literals so a line reads as the guest prints it.
    kernel = re.sub(r'"\s*\n\s*"', "",
                    open(os.path.join(REPO_ROOT, "kernel", "kernel.c")).read())
    seen = {}
    for p in PAYLOADS:
        for m in p["markers"]:
            check(m in required, "%s: %r is not a required marker" % (p["name"], m))
            check(m not in seen, "%r belongs to %s and %s" % (m, seen.get(m), p["name"]))
            seen[m] = p["name"]
        check(not p.get("skip") or p["skip"] in kernel,
              "%s: the kernel does not print %r" % (p["name"], p.get("skip")))
        check(p["built"] == ALWAYS or all(g and all(not x.startswith("/") for x in g)
                                         for g in p["built"]),
              "%s: built paths are relative to the checkout" % p["name"])
        check(not p.get("parent") or p["parent"] in BY_NAME,
              "%s: no parent %s" % (p["name"], p.get("parent")))
    check(len(BY_NAME) == len(PAYLOADS), "two payloads share a name")
    # Not a payload marker, and so never skippable.
    for m in ("[init] PID 1 spawned", "[m146] a descriptor belongs to the process:",
              "[m167] a process outliving its first thread:"):
        check(m in required and m not in seen, "%r should be required and payload-free" % m)

    scratch = tempfile.mkdtemp(prefix="leanos-battery-payloads-")
    try:
        root = os.path.join(scratch, "host")

        def fake_built(rel, at=None):
            path = os.path.join(at or root, resolve_built(rel))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(b"built on this host\n")
            return path

        node, v8 = BY_NAME["node"], BY_NAME["chromiumv8"]
        all_absent = {"image": "x", "unreadable": None,
                      "absent": {p["name"]: "%s not on x" % p["name"] for p in PAYLOADS}}
        skip_log = "\n".join(p["skip"] for p in PAYLOADS) + "\n"

        def expect(payload, state, log, want, what, at=None):
            got = decide(payload, state, log, at or root)[0]
            check(got == want, "%s: decided %s, should be %s" % (what, got, want))

        expect(node, all_absent, skip_log, "skip",
               "absent, never built here, and the machine said it skipped")
        expect(node, all_absent, skip_log.replace(node["skip"], ""), "fail",
               "absent and never built, but the machine never said it skipped")
        expect(node, dict(all_absent, absent={}), skip_log, "fail",
               "on the image with its marker missing")
        expect(node, dict(all_absent, unreadable="no superblock"), skip_log, "fail",
               "an image this reader could not read")
        expect(node, None, skip_log, "fail", "no reading of the image at all")
        expect(BY_NAME["mathltest"], all_absent, skip_log, "fail",
               "mathltest, which this tree builds on every host")
        expect(BY_NAME["toybox"], all_absent, skip_log, "fail",
               "toybox, which this tree builds on every host")
        # A child is the parent's business while the parent is absent - with
        # no line of its own - and its own once the parent is there.
        gnucxx = BY_NAME["gnucxx"]
        expect(gnucxx, all_absent, BY_NAME["cxxtest"]["skip"], "skip",
               "gnucxx under an absent /bin/cxxtest, on the parent's word")
        parent_there = dict(all_absent, absent={k: v for k, v in all_absent["absent"].items()
                                                if k != "cxxtest"})
        expect(gnucxx, parent_there, BY_NAME["cxxtest"]["skip"], "fail",
               "gnucxx with /bin/cxxtest present and no line of its own")
        expect(gnucxx, parent_there, gnucxx["skip"], "skip",
               "gnucxx with /bin/cxxtest present, on its own line")
        # ...but a child this host built is graded by its own build, parent
        # or no parent: built here and absent from the image is M113 even
        # where the parent was never built (build-dynamic.sh's throwmain
        # without cxx-test.sh, build-thirdparty.sh without gcc-test.sh's own
        # gcctest). Each on a host that built that child and nothing else.
        children = [p for p in PAYLOADS if p.get("parent")]
        check(sorted(p["name"] for p in children) ==
              ["bzip2+gnuhello", "cxxlib", "gnucxx", "throwmain"],
              "the children are %s" % sorted(p["name"] for p in children))
        for child in children:
            only = os.path.join(scratch, "only-" + re.sub(r"\W", "_", child["name"]))
            for group in child["built"]:
                fake_built(group[0], only)
            parent = BY_NAME[child["parent"]]
            check(not built_here(parent, only), "%s's host built its parent" % child["name"])
            expect(parent, all_absent, skip_log, "skip",
                   "%s, never built on a host that built only %s" % (parent["name"], child["name"]),
                   only)
            expect(child, all_absent, skip_log, "fail",
                   "%s built here, absent, under an absent %s nobody built (M113)"
                   % (child["name"], parent["name"]), only)
            on_image = {k: v for k, v in all_absent["absent"].items() if k != child["name"]}
            expect(child, dict(all_absent, absent=on_image), skip_log, "skip",
                   "%s built here and ON the image, under an absent %s: the parent's word"
                   % (child["name"], parent["name"]), only)

        # Built here: M113. Then the same with the artefact gone again.
        fake_built("build/chromium/src/out/ElectronNode/node")
        expect(node, all_absent, skip_log, "fail",
               "absent from the image and built on this host (M113)")
        expect(v8, all_absent, skip_log, "skip", "another payload, not built here")
        # LEANOS_CHROMIUM_OUT is read when the question is asked, by the very
        # table entries both the battery and the input suite hold.
        saved_out = os.environ.get("LEANOS_CHROMIUM_OUT")
        os.environ["LEANOS_CHROMIUM_OUT"] = "SelfTestElsewhere"
        try:
            fake_built("build/chromium/src/out/SelfTestElsewhere/v8test")
            expect(v8, all_absent, skip_log, "fail",
                   "built under LEANOS_CHROMIUM_OUT, which the installers read")
        finally:
            if saved_out is None:
                del os.environ["LEANOS_CHROMIUM_OUT"]
            else:
                os.environ["LEANOS_CHROMIUM_OUT"] = saved_out
        expect(v8, all_absent, skip_log, "skip",
               "built only under another LEANOS_CHROMIUM_OUT than this run's")
        expect(BY_NAME["chrome"], all_absent, skip_log, "skip",
               "//chrome with no content_shell (install-browser.sh needs both)")
        fake_built(_chromium_out("content_shell"))
        expect(BY_NAME["chrome"], all_absent, skip_log, "skip",
               "content_shell built but not //chrome")
        fake_built(_chromium_out("chrome"))
        expect(BY_NAME["chrome"], all_absent, skip_log, "fail", "both built")
        os.makedirs(os.path.join(root, "build/python/build"), exist_ok=True)
        expect(BY_NAME["python"], all_absent, skip_log, "skip", "python not built")
        fake_built("build/python/build/python.exe")
        expect(BY_NAME["python"], all_absent, skip_log, "fail",
               "python.exe (macOS's BUILDEXE) built")

        # grade(): a marker no payload owns is never skipped.
        skipped, failed = grade(["[init] PID 1 spawned"] + v8["markers"], all_absent,
                                skip_log, root)
        check([m for m, _ in failed] == ["[init] PID 1 spawned"],
              "grade failed %r, should fail only the payload-free marker" % failed)
        check(len(skipped) == 1 and skipped[0][2] == v8["markers"],
              "grade skipped %r, should be chromiumv8's two markers" % skipped)

        # The reader, on images whose contents this test chose: a fresh
        # filesystem with nothing on it, and the same with a payload written
        # on, and with an EMPTY payload (absent, as the kernel's node check
        # and qemu_input agree).
        put = os.path.join(REPO_ROOT, "build", "leanfs-put")
        if not os.access(put, os.X_OK):
            subprocess.call(["make", "-s", "-C", REPO_ROOT, "leanfs-put"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Sparse, and as large as the image the build makes (where there is
        # one) so the filesystem leanfs-put lays down fits in it.
        real = os.environ.get("LEANOS_IMAGE") or os.path.join(REPO_ROOT, "build", "os-image.bin")
        size = os.path.getsize(real) if os.path.isfile(real) else 8 << 30

        def fresh_image(path):
            with open(path, "wb") as f:
                f.truncate(size)
            rc = subprocess.call([put, path, full, "/bin/init"],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            check(rc == 0, "leanfs-put could not make a fresh image (%d)" % rc)

        image = os.path.join(scratch, "fresh.bin")
        full = os.path.join(scratch, "full")
        with open(full, "wb") as f:
            f.write(b"\x7fELF probe\n")
        empty = os.path.join(scratch, "empty")
        open(empty, "wb").close()
        fresh_image(image)
        state = probe(image)
        check(not state["unreadable"], "a fresh image read as unreadable: %s" % state["unreadable"])
        check(set(state["absent"]) == set(BY_NAME),
              "a fresh image has %s on it" % sorted(set(BY_NAME) - set(state["absent"])))
        subprocess.call([put, image, full, "/bin/chromiumv8"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.call([put, image, empty, "/bin/node"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        state = probe(image)
        check("chromiumv8" not in state["absent"], "/bin/chromiumv8 written and read as absent")
        check("node" in state["absent"], "an EMPTY /bin/node read as present")
        state = probe(os.path.join(scratch, "no-such-image.bin"))
        check(bool(state["unreadable"]), "a missing image was read")

        # And the harness itself, end to end on a log this test wrote: every
        # marker that is not a payload's, every payload's skip line, and the
        # fresh image - through tools/qemu-serial-test.sh's grade-only mode,
        # so the shell's wiring is what is graded, not just this file.
        fresh = os.path.join(scratch, "nothing.bin")
        fresh_image(fresh)
        owned = marker_owner()
        base_log = "\n".join([m for m in required if m not in owned] +
                             [p["skip"] for p in PAYLOADS]) + "\n"

        # Grade-only must never boot. If it regressed, these calls would be a
        # real battery of a fake image - 1500 s each, in the --fast tier - so
        # three things stand in the way, none of which trusts the script being
        # graded: the first qemu-system-x86_64 on PATH is a stub that records
        # that it was asked and exits; the ceiling argument is a few seconds,
        # not the default; and the whole call, in a session of its own, is
        # killed (that process group only - everything in it was started
        # here) when it runs past GRADE_ONLY_TIMEOUT. Any of them is a FAIL.
        stub_bin = os.path.join(scratch, "no-qemu")
        os.makedirs(stub_bin)
        booted = os.path.join(scratch, "booted")
        stub = os.path.join(stub_bin, "qemu-system-x86_64")
        with open(stub, "w") as f:
            f.write('#!/bin/sh\necho "$0 $*" >> "%s"\nexit 1\n' % booted)
        os.chmod(stub, 0o755)

        def grade_only(log_text, host, script, timeout):
            """(returncode, output, problem): problem is why this was not a
            grade-only run - it timed out, or it started a machine."""
            log = os.path.join(scratch, "serial.log")
            with open(log, "w") as f:
                f.write(log_text)
            env = dict(os.environ, LEANOS_IMAGE=fresh, LEANOS_SERIAL_GRADE_LOG=log,
                       LEANOS_PAYLOAD_HOST_ROOT=host,
                       PATH=stub_bin + os.pathsep + os.environ.get("PATH", ""))
            proc = subprocess.Popen([script, str(GRADE_ONLY_CEILING)],
                                    cwd=REPO_ROOT, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, start_new_session=True)
            problem = None
            try:
                raw, _ = proc.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except OSError:  # it ended between the timeout and here
                    pass
                raw, _ = proc.communicate()
                problem = "was still running after %d s and was killed" % timeout
            finally:
                # Interrupted while waiting (^C), the session it runs in is
                # still this test's to end; it is not left booting.
                if proc.returncode is None:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except OSError:
                        pass
            if os.path.exists(booted):
                with open(booted) as f:
                    problem = "started a machine (%s)" % f.read().strip()
                os.unlink(booted)
            return proc.returncode, raw.decode("utf-8", "replace"), problem

        def battery(log_text, host, what, want_rc, want_text):
            rc, out, problem = grade_only(log_text, host, os.path.join(HERE, "qemu-serial-test.sh"),
                                          GRADE_ONLY_TIMEOUT)
            if problem:
                check(False, "harness, %s: grade-only %s - it must grade the log and "
                             "boot nothing:\n%s" % (what, problem, out[-1500:]))
                return
            ok = rc == want_rc and all(t in out for t in want_text)
            check(ok, "harness, %s: exit %d (wanted %d), output lacking %r:\n%s"
                  % (what, rc, want_rc, [t for t in want_text if t not in out], out[-1500:]))

        # The guard, on scripts that do what a regressed grade-only mode would:
        # one starts a machine, one never finishes (a child of its own holding
        # the output open, as qemu would). Both have to be caught, the second
        # within the timeout it is given and with nothing of its left running.
        def fake_harness(name, body):
            path = os.path.join(scratch, name)
            with open(path, "w") as f:
                f.write("#!/bin/sh\n" + body)
            os.chmod(path, 0o755)
            return path

        # (-version: were the stub ever not first on PATH, the real qemu
        # answers and exits, and this check fails rather than hangs.)
        _rc, _out, problem = grade_only("", scratch, fake_harness(
            "boots.sh", "qemu-system-x86_64 -version >/dev/null\n"
                        "echo PASS: nothing booted\nexit 0\n"), GRADE_ONLY_TIMEOUT)
        check(bool(problem) and "started a machine" in problem,
              "a harness that ran qemu-system-x86_64 in grade-only mode was not caught (%s)"
              % problem)
        pidfile = os.path.join(scratch, "sleeper.pid")
        _rc, _out, problem = grade_only("", scratch, fake_harness(
            "hangs.sh", "sleep 600 &\necho $! > '%s'\nwait\n" % pidfile), 2)
        check(bool(problem) and "killed" in problem,
              "a grade-only run that never finished was not cut off (%s)" % problem)
        try:
            with open(pidfile) as f:
                sleeper = int(f.read().strip())
            # Dead, it may still be a moment from being reaped by whoever
            # inherited it; a few seconds before calling it left running.
            deadline = time.time() + 5
            while True:
                try:
                    os.kill(sleeper, 0)
                except OSError:
                    break
                if time.time() > deadline:
                    check(False, "the cut-off run left its child %d running" % sleeper)
                    os.kill(sleeper, signal.SIGKILL)  # this test's own child, by PID
                    break
                time.sleep(0.1)
        except (OSError, ValueError):
            check(False, "the hanging fake harness never started its child")

        # The browser's "built here" is one table: the input suite's PAYLOADS
        # hold battery_payloads.BROWSER_BUILT's own entries, and decide with
        # built_paths - so both answer alike for a host that built only
        # //chrome (a skip: install-browser.sh needs content_shell) and for
        # one that built both, under a LEANOS_CHROMIUM_OUT set at run time.
        import qemu_input
        import qemu_input_suite
        tables = [id(g) for g in BROWSER_BUILT.values()]
        for name, (_req, _how, built) in sorted(qemu_input_suite.PAYLOADS.items()):
            check(id(built) in tables,
                  "input suite %s: its built-here rule is not battery_payloads.BROWSER_BUILT's"
                  % name)
        for name in ("browser", "chrome", "content_shell"):
            check(id(BY_NAME[name]["built"]) in tables,
                  "battery %s: its built-here rule is not BROWSER_BUILT's" % name)
        saved = (os.environ.get("LEANOS_CHROMIUM_OUT"),
                 os.environ.get("LEANOS_PAYLOAD_HOST_ROOT"), qemu_input.IMAGE)
        try:
            os.environ["LEANOS_CHROMIUM_OUT"] = "SelfTestBrowser"
            qemu_input.IMAGE = fresh
            for label, artefacts, want_shell, want_chrome in (
                    ("only //chrome", ["chrome"], "skip", "skip"),
                    ("content_shell and //chrome", ["content_shell", "chrome"],
                     "fail", "fail")):
                host = os.path.join(scratch, "browser-" + "-".join(artefacts))
                for a in artefacts:
                    fake_built(_chromium_out(a), host)
                os.environ["LEANOS_PAYLOAD_HOST_ROOT"] = host
                suite = qemu_input_suite.payload_decisions(
                    sorted(qemu_input_suite.PAYLOADS), quiet=True)
                for name in sorted(qemu_input_suite.PAYLOADS):
                    want = want_chrome if "new_tab" in name else want_shell
                    got = suite.get(name, ("runs", ""))[0]
                    check(got == want, "input suite %s, %s built: decided %s, should be %s"
                          % (name, label, got, want))
                for name, want in (("browser", want_shell), ("content_shell", want_shell),
                                   ("chrome", want_chrome)):
                    got = decide(BY_NAME[name], all_absent, skip_log, host)[0]
                    check(got == want, "battery %s, %s built: decided %s, should be %s"
                          % (name, label, got, want))
        finally:
            for var, value in (("LEANOS_CHROMIUM_OUT", saved[0]),
                               ("LEANOS_PAYLOAD_HOST_ROOT", saved[1])):
                if value is None:
                    os.environ.pop(var, None)
                else:
                    os.environ[var] = value
            qemu_input.IMAGE = saved[2]

        empty_host = os.path.join(scratch, "empty-host")
        os.makedirs(empty_host)
        n_owned = len(owned)
        # mathltest and toybox are on no image here and are never skipped, so
        # their markers are written into the log as a host that ran
        # tools/math-long-double-test.sh and make toybox would see them.
        always = [m for p in PAYLOADS if p["built"] == ALWAYS for m in p["markers"]]
        ok_log = base_log + "\n".join(always) + "\n"
        n_skip = n_owned - len(always)
        battery(ok_log, empty_host, "nothing built, nothing on the image", 0,
                ["SKIPPED: %d/%d" % (n_skip, len(required)),
                 "PASS: %d/%d required boot markers found, %d skipped"
                 % (len(required) - n_skip, len(required), n_skip)])
        battery(base_log, empty_host, "mathltest and toybox missing", 1,
                ["mathltest: done", "tools/math-long-double-test.sh"])
        battery(ok_log, root, "node, //chrome and others built on this host", 1,
                ["[m223] Node.js ran 8 checks, 0 failed", "(M113)"])
        battery(ok_log, os.path.join(scratch, "only-throwmain"),
                "throwmain built on this host, /bin/cxxtest built nowhere", 1,
                ["FAIL: 1/%d" % len(required), "- [m97] and across a shared object:",
                 "build/dynamic/throwmain) - the image was recreated", "(M113)"])
        battery(ok_log.replace(node["skip"], ""), empty_host,
                "the machine never said node skipped", 1, ["never said"])
        battery(ok_log.replace("[smp] self-test passed.", ""), empty_host,
                "a payload-free marker missing", 1, ["- [smp] self-test passed."])
        battery(ok_log + "*** KERNEL PANIC: test\n", empty_host,
                "a panic beside the skips", 1, ["FAIL: kernel panicked"])
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        print("FAIL: the battery's payload skips did not hold:")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("PASS: %d payloads, %d markers; a marker is skipped only for a payload that is "
          "not on the image, was not built here and that the machine said it skipped - "
          "built here fails (M113, a child on its own build too), on the image fails, "
          "unread fails, a panic fails; the browser's built-here table is the input "
          "suite's; grade-only booted nothing and finished inside %d s."
          % (len(PAYLOADS), len(marker_owner()), GRADE_ONLY_TIMEOUT))
    return 0


def main(argv):
    args = argv[1:]
    if args == ["--self-test"]:
        return self_test()
    opts = {}
    cmd = args[0] if args else ""
    rest = args[1:]
    while len(rest) >= 2 and rest[0].startswith("--"):
        opts[rest[0][2:]] = rest[1]
        rest = rest[2:]
    if cmd == "probe" and not rest and "image" in opts and "state" in opts:
        return _cmd_probe(opts["image"], opts["state"])
    if cmd == "grade" and not rest and {"state", "log", "total"} <= set(opts):
        return _cmd_grade(opts["state"], opts["log"], int(opts["total"]), opts.get("summary"))
    print("usage: battery_payloads.py probe --image IMG --state FILE\n"
          "       battery_payloads.py grade --state FILE --log LOG --total N "
          "[--summary FILE] < missing\n"
          "       battery_payloads.py --self-test", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
