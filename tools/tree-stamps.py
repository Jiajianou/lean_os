#!/usr/bin/env python3
import os
import subprocess
import sys

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
    if len(sys.argv) != 3 or sys.argv[1] not in ("snapshot", "compare"):
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    now = stamps(root)
    if sys.argv[1] == "snapshot":
        with open(sys.argv[2], "w") as f:
            for rel in sorted(now):
                v = now[rel]
                f.write("%s\t%s\n" % ("-" if v is None else "%d %d" % v, rel))
        return 0

    before = {}
    with open(sys.argv[2]) as f:
        for line in f:
            stamp, rel = line.rstrip("\n").split("\t", 1)
            before[rel] = None if stamp == "-" else tuple(int(x) for x in stamp.split())
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
