#!/usr/bin/env python3
"""tools/tree-stamps.py - M116: a test run must leave the tree as it found it.

usage: tree-stamps.py snapshot|compare FILE

    tree-stamps.py snapshot FILE    record every tracked file's mtime and size
    tree-stamps.py compare  FILE    exit 1, naming them, if any has changed

tools/run-tests.sh takes a snapshot before every stage and compares after
it. A timestamp and not only the contents, because in a Makefile a new
mtime IS a change - M116's commit message has the whole story.
"""
import os
import subprocess
import sys

# A constant rather than a line of __doc__: a cleanup once stripped every
# docstring in tools/, and the usage message became an AttributeError.
USAGE = "usage: tree-stamps.py snapshot|compare FILE"

def stamps(root):
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, check=True,
                         stdout=subprocess.PIPE).stdout
    table = {}
    for rel in out.split(b"\0"):
        if not rel:
            continue
        path = os.path.join(root, rel.decode())
        try:
            st = os.lstat(path)
            table[rel.decode()] = (st.st_mtime_ns, st.st_size)
        except OSError:
            table[rel.decode()] = None
    return table

def main():
    if len(sys.argv) == 2 and sys.argv[1] in ("-h", "--help"):
        print(USAGE)
        return 0
    if len(sys.argv) != 3 or sys.argv[1] not in ("snapshot", "compare"):
        print(USAGE, file=sys.stderr)
        return 2
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    now = stamps(root)
    if sys.argv[1] == "snapshot":
        try:
            with open(sys.argv[2], "w") as f:
                for rel in sorted(now):
                    v = now[rel]
                    f.write("%s\t%s\n" % ("-" if v is None else "%d %d" % v, rel))
        except OSError as e:
            print("tree-stamps: cannot write %s: %s" % (sys.argv[2], e.strerror),
                  file=sys.stderr)
            return 2
        return 0

    # A snapshot that cannot be read is a usage error (2), not "the stage
    # changed something" (1): run-tests.sh fails the stage either way, but
    # the message should say which.
    before = {}
    try:
        with open(sys.argv[2]) as f:
            for line in f:
                stamp, rel = line.rstrip("\n").split("\t", 1)
                before[rel] = None if stamp == "-" else tuple(int(x) for x in stamp.split())
    except OSError as e:
        print("tree-stamps: cannot read %s: %s" % (sys.argv[2], e.strerror),
              file=sys.stderr)
        return 2
    except ValueError:
        print("tree-stamps: %s is not a snapshot this wrote" % sys.argv[2],
              file=sys.stderr)
        return 2
    changed = sorted(rel for rel in set(before) | set(now)
                     if before.get(rel) != now.get(rel))
    if not changed:
        return 0
    print("tree-stamps: this stage changed %d tracked file(s) - a test must "
          "leave the tree as it found it, because `make` reads a new mtime "
          "as a new file:" % len(changed), file=sys.stderr)
    for rel in changed[:12]:
        print("  %s" % rel, file=sys.stderr)
    if len(changed) > 12:
        print("  ... and %d more" % (len(changed) - 12), file=sys.stderr)
    return 1

if __name__ == "__main__":
    sys.exit(main())
