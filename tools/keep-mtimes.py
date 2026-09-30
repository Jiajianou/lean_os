#!/usr/bin/env python3
"""keep-mtimes.py save|restore <root> <state-file> [paths...]

M200. tools/chromium-test.sh and tools/build-chromium.sh reset every file the
Chromium patch series touches and apply the series again - on purpose, so that
"is this patch already applied" never needs an answer. Resetting and patching
writes each of those files, and Chromium's build takes the new modification
time for a change: after every default tier the next /bin/chrome build
recompiled every object that includes a patched header, for files whose
contents had not changed at all. That was a relink turning into hours.

`save` records each path's contents and modification time; `restore` puts the
old time back on every path whose contents came out byte-identical, and leaves
alone any that really changed. Paths that do not exist are skipped both ways.
"""
import hashlib
import json
import os
import sys


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main(argv):
    if len(argv) < 4 or argv[1] not in ("save", "restore"):
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    mode, root, state = argv[1], argv[2], argv[3]
    if mode == "save":
        record = {}
        for rel in argv[4:]:
            path = os.path.join(root, rel)
            if os.path.isfile(path):
                st = os.stat(path)
                record[rel] = [digest(path), st.st_mtime_ns, st.st_atime_ns]
        with open(state, "w") as f:
            json.dump(record, f)
        return 0
    try:
        with open(state) as f:
            record = json.load(f)
    except (OSError, ValueError):
        return 0
    kept = 0
    for rel, (sha, mtime_ns, atime_ns) in record.items():
        path = os.path.join(root, rel)
        if os.path.isfile(path) and digest(path) == sha:
            os.utime(path, ns=(atime_ns, mtime_ns))
            kept += 1
    print("keep-mtimes: %d of %d patched file(s) unchanged, their times put back" % (kept, len(record)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
