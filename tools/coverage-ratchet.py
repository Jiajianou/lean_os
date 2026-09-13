#!/usr/bin/env python3

import re
import subprocess
import sys

def parse_report(path):
    out = {}
    with open(path) as f:
        for line in f:
            cols = line.split()
            if len(cols) < 10 or cols[0] in ("Filename", "TOTAL") or set(cols[0]) == {"-"}:
                continue
            pcts = [c for c in cols if c.endswith("%")]
            if len(pcts) < 3:
                continue
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
                    where = parts[2] if len(parts) >= 3 and parts[2] else "unknown"
                    out[parts[0]] = (float(parts[1]), where)
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

    missing = sorted(set(floor) - set(got))
    fell = []
    rose = []
    new = []
    for name, pct in sorted(got.items()):
        if name not in floor:
            new.append((name, pct))
        elif pct < floor[name][0] - 0.05:
            fell.append((name, pct, floor[name][0], floor[name][1]))
        elif pct > floor[name][0] + 0.05:
            rose.append((name, pct, floor[name][0]))

    for name, pct in new:
        print("  new     %-28s %6.2f%%  (no floor yet - add a row to %s)"
              % (name, pct, floor_path))
    for name, pct, was in rose:
        print("  raised  %-28s %6.2f%%  (floor %.2f%% - raise it, and say so)"
              % (name, pct, was))
    for name, pct, was, where in fell:
        print("  FELL    %-28s %6.2f%%  (floor %.2f%%, set at %s)"
              % (name, pct, was, where))

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

    if new:
        print("\ncoverage-ratchet: %d file(s) in the report have no floor. "
              "Add a row to %s for each - the measured number is printed "
              "above and is what a first floor should be. A file the host "
              "tier builds and this table does not name is a file whose "
              "coverage nothing constrains." % (len(new), floor_path))
        return 1
    if rose:
        print("\ncoverage-ratchet: nothing fell. %d file(s) are above their "
              "floor; raising them is how the ratchet ratchets." % len(rose))
    else:
        print("\ncoverage-ratchet: nothing fell.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
