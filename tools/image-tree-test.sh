#!/usr/bin/env bash
# tools/image-tree-test.sh - M93 (second attempt): does a tree a host tool
# built into a leanfs image survive the trip onto the machine?
#
# ---- what this grades and why the other harnesses cannot ---------------
#
# M93's fourth bullet asked for a host-side image builder, on the grounds
# that kernel/proc/embed_programs.asm is the only road onto this disk and
# a source tree is not something that can be incbin'd into a kernel. The
# builder is tools/leanfs-put.c's -r mode. Grading it takes two claims and
# neither instrument already here can make both:
#
#   1. The image holds the tree it was given. Checked on the host, by
#      tools/leanfs-fsck.py --compare-tree, which is the reader that does
#      not share the writer's assumptions (see its header for why that
#      independence is worth a second implementation).
#   2. The machine can read it. Checked from inside, by kernel.c's
#      image-manifest self-test, which walks the tree and hashes every
#      byte of it against what the builder wrote down.
#
# The first without the second is a well-formed image nothing can open.
# The second without the first is a machine agreeing with a manifest that
# may describe a tree the host never had. The point of this script is that
# both run against the same image.
#
# ---- the tree ---------------------------------------------------------
#
# By default a synthetic one, generated below, whose contents are chosen
# to be the things a real source tarball has and a hand-written test does
# not: an empty file, a file past one block, a file past the direct-block
# range, a directory big enough to need an indirect block, a symlink, a
# hard link, a name at leanfs's 255-byte maximum, and non-ASCII bytes in a
# name. Each one is a case the builder got wrong at some point during M93
# and would have shipped without.
#
# --tree DIR runs it against a real tree instead - a source tarball
# unpacked - which is what this exists for in the end. The synthetic one
# is the version that runs in a minute.
#
# Usage:
#   tools/image-tree-test.sh                 # synthetic tree, host checks + boot
#   tools/image-tree-test.sh --tree ~/gcc-15.1.0 --at /src
#   tools/image-tree-test.sh --no-boot       # host checks only (seconds)
set -euo pipefail

cd "$(dirname "$0")/.."

TREE=""
AT="/src"
BOOT=1
# The battery itself is about 190s and this image adds the manifest walk
# on top of it. Measured rather than guessed - see the milestone notes for
# what the walk costs at four thousand files.
BOOT_SECONDS="${BOOT_SECONDS:-300}"

while [ $# -gt 0 ]; do
  case "$1" in
    --tree)    TREE="$2"; shift 2 ;;
    --at)      AT="$2"; shift 2 ;;
    --no-boot) BOOT=0; shift ;;
    -h|--help) sed -n '1,45p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/leanos-image-tree.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

echo "== building the image and the tool"
make -s all
make -s leanfs-put

if [ -z "$TREE" ]; then
  TREE="$WORK/tree"
  echo "== generating a synthetic tree"
  mkdir -p "$TREE/pkg/src/deep/deeper/deepest"
  # Empty. The single-file mode refused one of these for eight milestones
  # ("local-file is empty, unreadable"), and a Python package is mostly
  # empty __init__.py.
  : > "$TREE/pkg/__init__.py"
  printf 'int main(void) { return 0; }\n' > "$TREE/pkg/src/main.c"
  # Past one 4 KiB block, and past the sixteen direct blocks, so the
  # builder's block map is walked into its indirect table by real data
  # rather than by a self-test's pattern.
  head -c 200000 /dev/urandom > "$TREE/pkg/src/blob.bin"
  head -c 9000 /dev/urandom > "$TREE/pkg/src/deep/deeper/deepest/nested.bin"
  # A directory past sixteen blocks of records, which is where a directory
  # stops fitting in its direct blocks and starts needing an indirect one.
  # Nothing in this project had ever made one until the builder did - and
  # tools/leanfs-fsck.py could not read one, which is how that was found.
  #
  # 4000 rather than a rounder 20000, and the difference is four minutes of
  # boot: a name of this length packs 170 records to a block, so 2730 is
  # where the direct blocks run out and 4000 clears it with margin. The
  # property being tested is "past the direct range", and paying 5x for a
  # bigger number that tests the same property is how a suite gets slow
  # enough that people stop running it.
  mkdir -p "$TREE/pkg/many"
  awk 'BEGIN { for (i = 0; i < 4000; i++) printf "%05d\n", i }' > "$WORK/seq"
  while read -r i; do printf '%s\n' "$i" > "$TREE/pkg/many/entry-$i.txt"; done < "$WORK/seq"
  # A symlink stored as a symlink, and a hard link that stays one file.
  ln -s src/main.c "$TREE/pkg/link-to-main"
  ln "$TREE/pkg/src/main.c" "$TREE/pkg/second-name-for-main.c"
  # The awkward names: leanfs's maximum, and bytes above ASCII. M88's
  # UTF-8 work is not done, which is exactly why the *encoding* being
  # carried through unchanged is worth checking now.
  printf 'at the limit\n' > "$TREE/pkg/$(printf 'z%.0s' $(seq 1 255))"
  printf 'café\n' > "$TREE/pkg/lisez-moi-café.txt"
fi

IMG="$WORK/os-image.bin"
echo "== copying the image"
cp build/os-image.bin "$IMG"

# Every built-in program first, in the Makefile's own order. kernel.c's
# M22 self-test hardcodes "launcher slot 0 is hello, the first file ever
# seeded" and grades real pixels for it, so a tree that claims the first
# free inodes of a never-booted image shifts hello out of slot 0 and fails
# a self-test that has nothing to do with this milestone. `make preseed`
# says so in its own comment; this is that, against a copy.
echo "== preseeding the built-in programs"
for p in $(make -s print-user-programs); do
  build/leanfs-put "$IMG" "build/$p.elf" "/bin/$p" > /dev/null
done

echo "== writing the tree into the image"
build/leanfs-put -r "$IMG" "$TREE" "$AT"

echo "== checking the image, and comparing it to the tree it came from"
tools/leanfs-fsck.py "$IMG" --compare-tree "$TREE" --at "$AT"

# Built twice from one tree, byte for byte. The builder takes every
# timestamp from the host's stat and walks in sorted order precisely so
# this holds, and it did not until a `time(NULL)` in dir_add was found by
# this check failing at byte 4198409 - the root inode's mtime.
echo "== building it a second time and comparing the two images"
IMG2="$WORK/os-image-again.bin"
cp build/os-image.bin "$IMG2"
for p in $(make -s print-user-programs); do
  build/leanfs-put "$IMG2" "build/$p.elf" "/bin/$p" > /dev/null
done
build/leanfs-put -r "$IMG2" "$TREE" "$AT" > /dev/null
if ! cmp -s "$IMG" "$IMG2"; then
  echo "FAIL: two images built from one tree are not identical" >&2
  cmp "$IMG" "$IMG2" >&2 || true
  exit 1
fi
echo "the same tree built twice is the same image, byte for byte"

# M98: and the same tree put twice into ONE image is still that tree.
# -r became an installer when the native toolchain started re-running it
# over yesterday's image, and the seeder it used to be blind-added every
# name: the [m93] on-machine manifest check found exactly 2x the names,
# bytes and directories the host wrote. Replace-in-place is what -r does
# now, and this is the check that keeps it doing it - the compare-tree
# below fails on a single duplicated name.
echo "== putting the same tree into the same image a second time"
build/leanfs-put -r "$IMG" "$TREE" "$AT" > /dev/null
python3 tools/leanfs-fsck.py --quiet "$IMG" --compare-tree "$TREE" --at "$AT" || {
  echo "FAIL: re-putting a tree into the same image did not leave the same tree" >&2
  exit 1
}
echo "a re-put tree is the same tree - the installer case holds"

if [ "$BOOT" -eq 0 ]; then
  echo
  echo "PASS (host checks only; --no-boot was given, so nothing has read this image from inside)"
  exit 0
fi

echo
echo "== booting it, and reading the tree back from inside the machine"
LOG="$WORK/serial.log"
if LEANOS_IMAGE="$IMG" tools/qemu-serial-test.sh "$BOOT_SECONDS" > "$LOG" 2>&1; then
  boot_ok=1
else
  boot_ok=0
fi

MARKER="image-manifest self-test passed"
if grep -q "$MARKER" "$LOG"; then
  grep -A1 "a tree a host tool built into this image" "$LOG" | head -4
else
  echo "FAIL: the machine did not report reading the tree back." >&2
  echo "      (the whole battery's result was: $([ $boot_ok -eq 1 ] && echo pass || echo fail))" >&2
  tail -40 "$LOG" >&2
  exit 1
fi

if [ "$boot_ok" -ne 1 ]; then
  echo "FAIL: the tree was read back, but the rest of the boot battery did not pass." >&2
  tail -40 "$LOG" >&2
  exit 1
fi

echo
echo "PASS: the tree survived the host tool, the image, and the machine's own reader."
