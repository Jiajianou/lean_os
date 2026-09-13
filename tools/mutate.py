#!/usr/bin/env python3

import argparse
import os
import random
import re
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEST_CMD = ["make", "--no-print-directory", "test-fast", "TEST_SAN=0"]

INSTRUMENTS = {
    "user_space/libc/src/stdio.c": [["./tools/printf-test.sh"],
                                    ["./tools/stdio-test.sh"]],
    "user_space/libc/src/scanf.c": [["./tools/scanf-test.sh"]],
    "user_space/libc/src/regex.c": [["./tools/regex-test.sh"]],
}

TIMEOUT_S = 90

def mask_comments_and_strings(src):
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        elif c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif c in '"\'':
            quote = c
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == quote:
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        elif c == '#':
            j = src.find('\n', i)
            while j > 0 and src[j - 1] == '\\':
                j = src.find('\n', j + 1)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        else:
            i += 1
    return ''.join(out)

def _flip(mapping):
    return lambda m: mapping[m.group(0)]

OPERATORS = [
    ("relational", re.compile(r'(?<![<>=!-])(<=|>=|<|>)(?![<>=])'),
     _flip({'<': '<=', '<=': '<', '>': '>=', '>=': '>'})),

    ("equality", re.compile(r'(?<![<>=!])(==|!=)(?!=)'),
     _flip({'==': '!=', '!=': '=='})),

    ("logical", re.compile(r'(&&|\|\|)'),
     _flip({'&&': '||', '||': '&&'})),

    ("constant_plus_one", re.compile(r'(?<![\w.])(\d+)(?![\w.])'),
     lambda m: str(int(m.group(1)) + 1)),

    ("constant_minus_one", re.compile(r'(?<![\w.])([1-9]\d*)(?![\w.])'),
     lambda m: str(int(m.group(1)) - 1)),

    ("condition_true", re.compile(r'\bif\s*\('), lambda m: 'if (1 || ('),
    ("condition_false", re.compile(r'\bif\s*\('), lambda m: 'if (0 && ('),
]

NEEDS_CLOSE = {"condition_true", "condition_false"}

def find_condition_end(src, open_paren_idx):
    depth = 0
    i = open_paren_idx
    while i < len(src):
        if src[i] == '(':
            depth += 1
        elif src[i] == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1

EQUIVALENT = [
    ("libk.c", "for (size_t i = 0; i < n; i++)",
     "loop bound constant 0 -> 1 on an empty-range loop is unreachable "
     "in callers that never pass n == 0; kept out because the suite "
     "cannot distinguish it and neither can the caller"),
]

def is_equivalent(path, line_text):
    for f, l, _reason in EQUIVALENT:
        if f in path and l in line_text.strip():
            return True
    return False

class Mutant:
    def __init__(self, path, op, start, end, replacement, line_no, line_text):
        self.path = path
        self.op = op
        self.start = start
        self.end = end
        self.replacement = replacement
        self.line_no = line_no
        self.line_text = line_text.strip()
        self.verdict = None

    def apply(self, src):
        return src[:self.start] + self.replacement + src[self.end:]

    def __str__(self):
        return "%s:%d %s: %s" % (os.path.relpath(self.path, ROOT),
                                 self.line_no, self.op, self.line_text[:70])

def line_of(src, idx):
    return src.count('\n', 0, idx) + 1

def line_text_of(src, idx):
    start = src.rfind('\n', 0, idx) + 1
    end = src.find('\n', idx)
    return src[start:end if end > 0 else len(src)]

def mutants_for(path):
    with open(path) as f:
        src = f.read()
    masked = mask_comments_and_strings(src)
    out = []
    for name, pattern, repl in OPERATORS:
        for m in pattern.finditer(masked):
            start, end = m.start(), m.end()
            text = line_text_of(src, start)
            if is_equivalent(path, text):
                continue
            if name in NEEDS_CLOSE:
                close = find_condition_end(masked, end - 1)
                if close < 0:
                    continue
                cond = src[end:close]
                new = repl(m) + cond + ')'
                out.append(Mutant(path, name, start, close, new,
                                  line_of(src, start), text))
            else:
                out.append(Mutant(path, name, start, end, repl(m),
                                  line_of(src, start), text))
    return out

TEST_BIN = "build/tests/leanos-tests"

def drop_test_binary():
    try:
        os.remove(os.path.join(ROOT, TEST_BIN))
    except OSError:
        pass

def run_bounded(cmd):
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, start_new_session=True)
    try:
        _, err = p.communicate(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
        p.communicate()
        raise
    return subprocess.CompletedProcess(cmd, p.returncode, None, err)

def run_suite(extra=()):
    drop_test_binary()
    started = time.time()
    try:
        p = run_bounded(TEST_CMD)
    except subprocess.TimeoutExpired:
        return "killed-timeout", time.time() - started
    err = p.stderr.decode("utf-8", "replace")
    if p.returncode != 0 and ("error:" in err or "Error 1" not in err and
                              "error" in err.lower() and "test" not in err.lower()):
        if "error:" in err:
            return "build-failed", time.time() - started
    if p.returncode != 0:
        return "killed", time.time() - started
    for cmd in extra:
        try:
            q = run_bounded(cmd)
        except subprocess.TimeoutExpired:
            return "killed-timeout", time.time() - started
        if q.returncode != 0:
            qerr = q.stderr.decode("utf-8", "replace")
            if "could not compile" in qerr:
                return "build-failed", time.time() - started
            return "killed", time.time() - started
    return "survived", time.time() - started

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="sources to mutate")
    ap.add_argument("--limit", type=int, default=0,
                    help="sample this many mutants per file rather than all")
    ap.add_argument("--seed", type=int, default=1,
                    help="sampling seed, so a run is repeatable")
    ap.add_argument("--list", action="store_true",
                    help="print the mutants and exit without running anything")
    args = ap.parse_args()

    if not args.files:
        ap.error("no files given")

    rng = random.Random(args.seed)
    all_mutants = []
    for path in args.files:
        full = os.path.join(ROOT, path)
        found = mutants_for(full)
        if args.limit and len(found) > args.limit:
            found = rng.sample(found, args.limit)
            found.sort(key=lambda mu: mu.start)
        all_mutants.extend(found)

    if args.list:
        for mu in all_mutants:
            print(mu)
        print("\n%d mutants" % len(all_mutants))
        return 0

    baseline_extra = []
    for path in args.files:
        for cmd in INSTRUMENTS.get(path, []):
            if cmd not in baseline_extra:
                baseline_extra.append(cmd)
    sys.stdout.write("baseline: ")
    sys.stdout.flush()
    verdict, secs = run_suite(baseline_extra)
    if verdict != "survived":
        print("the suite does not pass on the unmutated tree (%s). "
              "Fix that first - every mutant below would be reported killed "
              "for the wrong reason." % verdict)
        return 2
    print("suite passes in %.1fs\n" % secs)

    originals = {}
    stamps = {}
    for path in args.files:
        full = os.path.join(ROOT, path)
        with open(full) as f:
            originals[full] = f.read()
        st = os.stat(full)
        stamps[full] = (st.st_atime_ns, st.st_mtime_ns)

    def put_back(full):
        with open(full, "w") as f:
            f.write(originals[full])
        os.utime(full, ns=stamps[full])

    def restore(*_a):
        for full in originals:
            put_back(full)
        drop_test_binary()

    signal.signal(signal.SIGINT, lambda *a: (restore(), sys.exit(130)))
    signal.signal(signal.SIGTERM, lambda *a: (restore(), sys.exit(143)))

    results = []
    try:
        for i, mu in enumerate(all_mutants, 1):
            with open(mu.path, "w") as f:
                f.write(mu.apply(originals[mu.path]))
            verdict, secs = run_suite(
                INSTRUMENTS.get(os.path.relpath(mu.path, ROOT), []))
            mu.verdict = verdict
            results.append(mu)
            mark = {"killed": ".", "killed-timeout": "T",
                    "survived": "S", "build-failed": "b"}[verdict]
            sys.stdout.write(mark)
            sys.stdout.flush()
            if i % 60 == 0:
                sys.stdout.write("  %d/%d\n" % (i, len(all_mutants)))
                sys.stdout.flush()
            put_back(mu.path)
    finally:
        restore()

    print("\n")
    report(results)
    return 0

def report(results):
    by_file = {}
    for mu in results:
        by_file.setdefault(mu.path, []).append(mu)

    print("%-28s %7s %7s %7s %9s" % ("file", "killed", "lived", "nobuild", "score"))
    print("-" * 62)
    total_k = total_s = 0
    for path, muts in sorted(by_file.items()):
        killed = sum(1 for m in muts if m.verdict.startswith("killed"))
        survived = sum(1 for m in muts if m.verdict == "survived")
        nobuild = sum(1 for m in muts if m.verdict == "build-failed")
        total_k += killed
        total_s += survived
        viable = killed + survived
        score = ("%6.1f%%" % (100.0 * killed / viable)) if viable else "     -"
        print("%-28s %7d %7d %7d %9s"
              % (os.path.relpath(path, ROOT), killed, survived, nobuild, score))
    print("-" * 62)
    viable = total_k + total_s
    print("%-28s %7d %7d %7s %9s"
          % ("TOTAL", total_k, total_s, "",
             ("%6.1f%%" % (100.0 * total_k / viable)) if viable else "-"))

    survivors = [m for m in results if m.verdict == "survived"]
    if survivors:
        print("\nSurvivors - each is a fault this suite cannot see:\n")
        for mu in survivors:
            print("  %s" % mu)
        print("\n%d survivor(s). A survivor in a well-covered file is the "
              "interesting kind: it is coverage without an assertion."
              % len(survivors))
    else:
        print("\nNo survivors.")

if __name__ == "__main__":
    sys.exit(main())
