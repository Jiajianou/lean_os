#!/usr/bin/env python3
"""tools/tree-stamps.py - M116: a test run must leave the tree as it found it.

    tree-stamps.py snapshot FILE    record every tracked file's mtime and size
    tree-stamps.py compare  FILE    exit 1, naming them, if any has changed

tools/run-tests.sh takes a snapshot before every stage and compares after
it, so a stage that touches a tracked file fails - by name, with the
files it touched.

Why a timestamp and not only the contents: in a Makefile a new mtime IS a
change. tools/iconv-test.sh regenerated user_space/libc/src/iconv_tables.c
in place to check it matched - the same bytes, a new mtime - and the next
`make all` rebuilt it, relinked all sixty-six programs, relinked the
kernel that embeds them and recreated the disk image. That printed 135 KB
of near-identical link lines into the terminal of whoever ran
tools/run-qemu.sh next, and deleted everything on their machine's
filesystem, after every test run since M100. `git status` was clean the
whole time, which is why nothing noticed: the one tool everybody checks
compares contents, and this bug had none. tools/mutate.py had the same
habit with the kernel's sources.

Tracked files only (`git ls-files`), so build output, logs and anything
else a stage is supposed to write never count.
"""
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
            table[rel.decode()] = None  # deleted in the working tree
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
