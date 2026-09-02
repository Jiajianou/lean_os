#!/usr/bin/env python3
"""tools/coverage-ratchet.py - Q11: coverage may not go down.

Q8 measured coverage and deliberately set no target, on the argument that
a target chosen before a measurement is how a project ends up testing
getters. That argument is about the *absolute* number and it does not
apply to the direction: whatever the number is today, a change that
lowers it should have to say so.

So this is a ratchet rather than a target. The floor per file lives in
tests/coverage-floor.tsv with the commit that set it, exactly like
tests/budgets.tsv, and raising one is a deliberate edit. Lowering one is
allowed too - deleting a test can be the right call - and is equally
deliberate, which is the whole point.

Usage:  tools/coverage-ratchet.py <llvm-cov report> <floor file>
"""

import re
import subprocess
import sys


def parse_report(path):
    """{file: line_coverage_percent} from an llvm-cov report."""
    out = {}
    with open(path) as f:
        for line in f:
            # Filename Regions Missed Cover Functions Missed Executed
            #   Lines Missed Cover Branches Missed Cover
            cols = line.split()
            if len(cols) < 10 or cols[0] in ("Filename", "TOTAL") or set(cols[0]) == {"-"}:
                continue
            pcts = [c for c in cols if c.endswith("%")]
            if len(pcts) < 3:
                continue
            # Region, function, line, branch - line coverage is the third.
            try:
                out[cols[0]] = float(pcts[2].rstrip("%"))
            except ValueError:
                continue
    return out


def parse_floor(path):
    out = {}
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 2:
                    out[parts[0]] = float(parts[1])
    except FileNotFoundError:
        pass
    return out


def commit():
    try:
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"],
                                       stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "unknown"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    report, floor_path = sys.argv[1], sys.argv[2]
    got = parse_report(report)
    floor = parse_floor(floor_path)

    if not got:
        print("coverage-ratchet: could not parse %s - no rows found" % report)
        return 2

    # M101: a floor with no row in the report is an error, not a silence.
    #
    # The names in the floor file are whatever llvm-cov printed, and
    # llvm-cov strips the longest common prefix of the sources it was
    # given - so adding one source from a different top-level directory
    # renames every other file in the report. When that happened, every
    # floor stopped matching, all ten files were reported as "new", and
    # the ratchet printed "nothing fell". It had stopped enforcing
    # anything and said so in a way that reads like success.
    #
    # This is the same failure Q11 built the ratchet against, one level
    # up: an instrument that cannot fail. A floor that names a file the
    # report does not have is either a drifted name or a deleted test,
    # and both are things a person has to decide about.
    missing = sorted(set(floor) - set(got))
    fell = []
    rose = []
    new = []
    for name, pct in sorted(got.items()):
        if name not in floor:
            new.append((name, pct))
        elif pct < floor[name] - 0.05:      # a hair of tolerance for rounding
            fell.append((name, pct, floor[name]))
        elif pct > floor[name] + 0.05:
            rose.append((name, pct, floor[name]))

    for name, pct in new:
        print("  new     %-28s %6.2f%%  (no floor yet - add a row to %s)"
              % (name, pct, floor_path))
    for name, pct, was in rose:
        print("  raised  %-28s %6.2f%%  (floor %.2f%% - raise it, and say so)"
              % (name, pct, was))
    for name, pct, was in fell:
        print("  FELL    %-28s %6.2f%%  (floor %.2f%%, set at %s)"
              % (name, pct, was, commit()))

    if missing:
        for name in missing:
            print("  MISSING %-28s (a floor row with no line in the report)"
                  % name)
        print("\ncoverage-ratchet: %d floor row(s) name a file the report does "
              "not contain. Either the test tier stopped building it, or the "
              "names drifted - llvm-cov strips the longest common prefix of "
              "the sources it is given, so adding one changes the rest. Fix "
              "the names in %s rather than deleting the rows."
              % (len(missing), floor_path))
        return 1

    if fell:
        print("\ncoverage-ratchet: %d file(s) below their floor. If this is "
              "deliberate - a test deleted on purpose - lower the floor in %s "
              "and the commit message will say why." % (len(fell), floor_path))
        return 1
    if rose:
        print("\ncoverage-ratchet: nothing fell. %d file(s) are above their "
              "floor; raising them is how the ratchet ratchets." % len(rose))
    else:
        print("\ncoverage-ratchet: nothing fell.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
