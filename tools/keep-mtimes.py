#!/usr/bin/env python3
"""keep-mtimes.py save|restore <root> <state-file> [paths...] | --self-test

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

M222. One checkout now builds two programs - the browser with this fork's
series, Electron with Electron's on top - so a file can be in one of two
states and goes back and forth between them. The record is therefore a
history, every content a path has been seen with and the time it had then,
and `restore` puts back the time that belongs to the content found. Keeping
only the last state would make every switch between the two builds look like
an edit to every file Electron touches, and the next build of either one a
rebuild of most of Chromium.
"""
import hashlib
import json
import os
import sys
import tempfile


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load(state):
    try:
        with open(state) as f:
            record = json.load(f)
    except (OSError, ValueError):
        return {}
    for rel, entry in list(record.items()):
        if isinstance(entry, list):
            record[rel] = {entry[0]: entry[1:]}
    return record


def save(root, state, paths):
    record = load(state)
    for rel in paths:
        path = os.path.join(root, rel)
        if os.path.isfile(path):
            st = os.stat(path)
            record.setdefault(rel, {})[digest(path)] = [st.st_mtime_ns,
                                                        st.st_atime_ns]
    with open(state, "w") as f:
        json.dump(record, f)


def restore(root, state):
    record = load(state)
    kept = 0
    for rel, seen in record.items():
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        times = seen.get(digest(path))
        if times:
            os.utime(path, ns=(times[1], times[0]))
            kept += 1
    return kept, len(record)


def self_test():
    problems = []
    with tempfile.TemporaryDirectory() as root:
        state = os.path.join(root, "state.json")
        path = os.path.join(root, "f.h")

        def write(text, when):
            with open(path, "w") as f:
                f.write(text)
            os.utime(path, ns=(when, when))

        write("browser", 1_000_000_000)
        save(root, state, ["f.h"])
        write("electron", 2_000_000_000)
        restore(root, state)
        if os.stat(path).st_mtime_ns != 2_000_000_000:
            problems.append("a changed file was given an old time")
        save(root, state, ["f.h"])
        write("browser", 3_000_000_000)
        restore(root, state)
        if os.stat(path).st_mtime_ns != 1_000_000_000:
            problems.append("switching back did not restore the first state's time")
        write("electron", 4_000_000_000)
        restore(root, state)
        if os.stat(path).st_mtime_ns != 2_000_000_000:
            problems.append("switching again did not restore the second state's time")

        with open(state, "w") as f:
            json.dump({"f.h": [digest(path), 5_000_000_000, 5_000_000_000]}, f)
        restore(root, state)
        if os.stat(path).st_mtime_ns != 5_000_000_000:
            problems.append("a state file in the M200 format was not read")
    for problem in problems:
        print("keep-mtimes self-test: " + problem)
    if not problems:
        print("keep-mtimes self-test: two states, switched three times, every time right")
    return 1 if problems else 0


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) < 4 or argv[1] not in ("save", "restore"):
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    mode, root, state = argv[1], argv[2], argv[3]
    if mode == "save":
        save(root, state, argv[4:])
        return 0
    kept, total = restore(root, state)
    print("keep-mtimes: %d of %d patched file(s) in a state seen before, their times put back" % (kept, total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
