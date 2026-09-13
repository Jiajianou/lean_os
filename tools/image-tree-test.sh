#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

TREE=""
AT="/src"
BOOT=1
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
  : > "$TREE/pkg/__init__.py"
  printf 'int main(void) { return 0; }\n' > "$TREE/pkg/src/main.c"
  head -c 200000 /dev/urandom > "$TREE/pkg/src/blob.bin"
  head -c 9000 /dev/urandom > "$TREE/pkg/src/deep/deeper/deepest/nested.bin"
  mkdir -p "$TREE/pkg/many"
  awk 'BEGIN { for (i = 0; i < 4000; i++) printf "%05d\n", i }' > "$WORK/seq"
  while read -r i; do printf '%s\n' "$i" > "$TREE/pkg/many/entry-$i.txt"; done < "$WORK/seq"
  ln -s src/main.c "$TREE/pkg/link-to-main"
  ln "$TREE/pkg/src/main.c" "$TREE/pkg/second-name-for-main.c"
  printf 'at the limit\n' > "$TREE/pkg/$(printf 'z%.0s' $(seq 1 255))"
  printf 'café\n' > "$TREE/pkg/lisez-moi-café.txt"
fi

IMG="$WORK/os-image.bin"
echo "== copying the image"
cp build/os-image.bin "$IMG"

echo "== preseeding the built-in programs"
for p in $(make -s print-user-programs); do
  build/leanfs-put "$IMG" "build/$p.elf" "/bin/$p" > /dev/null
done

echo "== writing the tree into the image"
build/leanfs-put -r "$IMG" "$TREE" "$AT"

echo "== checking the image, and comparing it to the tree it came from"
tools/leanfs-fsck.py "$IMG" --compare-tree "$TREE" --at "$AT"

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
