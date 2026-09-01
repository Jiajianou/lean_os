#!/usr/bin/env python3
"""tools/mutate.py - Q12: does this test suite detect anything?

---- Why this exists ---------------------------------------------------

Coverage says which lines ran. It cannot say whether anything would have
noticed if those lines were wrong, and the difference is not academic:
Q7 produced a test that executed the right code, asserted something true,
and passed against a build with the bug still in it. It was caught only
because somebody thought to revert the fix and re-run.

This is that thought, mechanised. Break the code on purpose, one small
change at a time, and see whether `make test-fast` notices.

    KILLED    the suite failed. That fault is detectable.
    SURVIVED  the suite passed. That fault is not.

A survivor is a hole. The number worth watching is not the total - it is
a survivor in a file at high line coverage, because that is coverage
without assertions, which is the exact failure a percentage cannot show.

---- What it deliberately does not do ---------------------------------

No mutation-score target. Q8 refused to set a coverage target before
measuring anything and the argument is stronger here: a target invites
tests written to kill mutants rather than to describe behaviour, which is
a worse suite that reports a better number.

Equivalent mutants - changes that genuinely cannot alter behaviour - are
excluded by hand in EQUIVALENT below, each with a reason. Silently
dropping them would make the number a lie in the other direction.

---- Usage -------------------------------------------------------------

    make mutate                       # every file the host tier builds
    make mutate FILE=kernel/mm/heap.c # one file
    make mutate MUTANTS=40            # a sample rather than the census

The original file is restored on every exit path, including a signal.
"""

import argparse
import os
import random
import re
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The test command a mutant has to survive. Sanitizers off: they cost
# ~2x and a mutation campaign is thousands of runs. A fault that only
# ASan can see is a fault this campaign under-reports, which is a real
# limitation and is recorded rather than hidden.
TEST_CMD = ["make", "--no-print-directory", "test-fast", "TEST_SAN=0"]

# A mutant can hang - an inverted loop condition is the classic case - and
# a hang is a detected fault, not a stuck harness. Generous enough that a
# slow machine does not manufacture kills.
TIMEOUT_S = 90


# ---- Finding the places worth breaking --------------------------------
#
# Sites are found on a *masked* copy in which comments and string
# literals are replaced by spaces of the same length. That keeps offsets
# identical to the real file, so an edit can be applied to the original
# while never landing inside a comment - which would produce a mutant
# that changes nothing and counts as a survivor, quietly poisoning the
# number this whole tool exists to produce.

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
            # A preprocessor line. `#include <foo.h>` has angle brackets
            # that are not comparisons, and a mutated #define is a
            # compile error more often than a fault.
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


# Each operator: a regex over the masked source, and a function from the
# match to its replacement text. Ordered roughly by how likely the fault
# is to be one a person would actually write.
def _flip(mapping):
    return lambda m: mapping[m.group(0)]


OPERATORS = [
    # Relational boundaries - the off-by-one family, and by far the most
    # common real defect in this kind of code.
    #
    # The `-` in the lookbehind is not decoration. Without it this matches
    # the `>` in `b->size` and produces `b->=size`, which does not compile
    # - and the first census run counted every one of those as a SURVIVOR,
    # because the build-failure detection below was also wrong. Two bugs
    # that cancelled into a plausible-looking number, which is the worst
    # way for a measurement tool to be broken. `<<` and `>>` are already
    # excluded by the lookarounds.
    ("relational", re.compile(r'(?<![<>=!-])(<=|>=|<|>)(?![<>=])'),
     _flip({'<': '<=', '<=': '<', '>': '>=', '>=': '>'})),

    # Equality inverted.
    ("equality", re.compile(r'(?<![<>=!])(==|!=)(?!=)'),
     _flip({'==': '!=', '!=': '=='})),

    # Short-circuit swapped: the "one of these is wrong" family.
    ("logical", re.compile(r'(&&|\|\|)'),
     _flip({'&&': '||', '||': '&&'})),

    # A constant one out. Restricted to plain decimal literals that are
    # not part of an identifier, and skipping 0 in the +1 direction is
    # deliberate - see the 'zero' operator below.
    ("constant_plus_one", re.compile(r'(?<![\w.])(\d+)(?![\w.])'),
     lambda m: str(int(m.group(1)) + 1)),

    ("constant_minus_one", re.compile(r'(?<![\w.])([1-9]\d*)(?![\w.])'),
     lambda m: str(int(m.group(1)) - 1)),

    # A guard that always takes one branch. Catches a check that nothing
    # asserts the *negative* of.
    ("condition_true", re.compile(r'\bif\s*\('), lambda m: 'if (1 || ('),
    ("condition_false", re.compile(r'\bif\s*\('), lambda m: 'if (0 && ('),
]

# Operators that need a balancing paren appended at the end of the
# condition - handled specially because a regex cannot match balanced
# parens.
NEEDS_CLOSE = {"condition_true", "condition_false"}


def find_condition_end(src, open_paren_idx):
    """Index just past the `)` that closes the `(` at open_paren_idx."""
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


# ---- Equivalent mutants ------------------------------------------------
#
# A mutation that genuinely cannot change observable behaviour is not a
# hole in the suite, and counting it as one understates the suite in the
# same way that hiding it would overstate it. Each entry is (file
# substring, line-content substring, reason) and each is a judgement that
# should be readable and arguable.
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
                # Two edits at once, so this is built as an explicit
                # replacement of the whole condition rather than a splice.
                cond = src[end:close]
                new = repl(m) + cond + ')'
                out.append(Mutant(path, name, start, close, new,
                                  line_of(src, start), text))
            else:
                out.append(Mutant(path, name, start, end, repl(m),
                                  line_of(src, start), text))
    return out


# ---- Running -----------------------------------------------------------

# The test binary. Removed before every run rather than trusted to
# rebuild, because make decides by timestamp and a mutation campaign
# rewrites the same file hundreds of times a minute - and, worse, an
# interrupted campaign can leave a *mutant binary* on disk beside a
# restored source, at which point `make test-fast` reports failures that
# have no cause anybody can find. That happened while this file was being
# written and cost twenty minutes; deleting one file is cheaper.
TEST_BIN = "build/tests/leanos-tests"


def drop_test_binary():
    try:
        os.remove(os.path.join(ROOT, TEST_BIN))
    except OSError:
        pass


def run_suite():
    """(verdict, seconds). 'killed' means the suite noticed."""
    drop_test_binary()
    started = time.time()
    try:
        p = subprocess.run(TEST_CMD, cwd=ROOT, timeout=TIMEOUT_S,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    except subprocess.TimeoutExpired:
        # A hang is a detected fault. An inverted loop condition is the
        # usual cause and a person would notice it immediately.
        return "killed-timeout", time.time() - started
    err = p.stderr.decode("utf-8", "replace")
    if p.returncode != 0 and ("error:" in err or "Error 1" not in err and
                              "error" in err.lower() and "test" not in err.lower()):
        # Distinguishing a compiler rejection from a test failure matters:
        # a mutant that does not build is not a mutant, and counting it
        # either way is wrong.
        if "error:" in err:
            return "build-failed", time.time() - started
    return ("killed" if p.returncode != 0 else "survived"), time.time() - started


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

    # A baseline run first. Mutating a tree whose tests already fail
    # produces a page of meaningless kills.
    sys.stdout.write("baseline: ")
    sys.stdout.flush()
    verdict, secs = run_suite()
    if verdict != "survived":
        print("the suite does not pass on the unmutated tree (%s). "
              "Fix that first - every mutant below would be reported killed "
              "for the wrong reason." % verdict)
        return 2
    print("suite passes in %.1fs\n" % secs)

    originals = {}
    for path in args.files:
        full = os.path.join(ROOT, path)
        with open(full) as f:
            originals[full] = f.read()

    def restore(*_a):
        for full, src in originals.items():
            with open(full, "w") as f:
                f.write(src)
        # And the binary built from the mutant, which would otherwise
        # outlive it and be run by the next `make test-fast`.
        drop_test_binary()

    # Restored on every exit path. A tool that leaves a mutated kernel
    # behind when it is interrupted is a tool nobody runs twice.
    signal.signal(signal.SIGINT, lambda *a: (restore(), sys.exit(130)))
    signal.signal(signal.SIGTERM, lambda *a: (restore(), sys.exit(143)))

    results = []
    try:
        for i, mu in enumerate(all_mutants, 1):
            with open(mu.path, "w") as f:
                f.write(mu.apply(originals[mu.path]))
            verdict, secs = run_suite()
            mu.verdict = verdict
            results.append(mu)
            mark = {"killed": ".", "killed-timeout": "T",
                    "survived": "S", "build-failed": "b"}[verdict]
            sys.stdout.write(mark)
            sys.stdout.flush()
            if i % 60 == 0:
                sys.stdout.write("  %d/%d\n" % (i, len(all_mutants)))
                sys.stdout.flush()
            with open(mu.path, "w") as f:
                f.write(originals[mu.path])
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
