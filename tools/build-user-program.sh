#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  echo "usage: $0 <source.c> [output-name]" >&2
  exit 1
fi
SRC="$1"
NAME="${2:-$(basename "$SRC" .c)}"

if [ ! -f "$SRC" ]; then
  echo "error: no such file: $SRC" >&2
  exit 1
fi

BUILD="build"
UOBJ="$BUILD/user_obj"
USER_LD="user_space/lib/user.ld"
CC="x86_64-elf-gcc"
LD="x86_64-elf-ld"

USER_CFLAGS=$(make -s print-USER_CFLAGS)
USER_LIBOBJS=$(make -s print-USER_LIBOBJS)

echo "Building user_space/lib prerequisites..."
make -s $USER_LIBOBJS "$USER_LD" >/dev/null

mkdir -p "$UOBJ"
echo "Compiling $SRC..."
$CC $USER_CFLAGS "$SRC" -o "$UOBJ/$NAME.o"

echo "Linking $BUILD/$NAME.elf..."
$LD -T "$USER_LD" -o "$BUILD/$NAME.elf" $USER_LIBOBJS "$UOBJ/$NAME.o"

echo "Built $BUILD/$NAME.elf - put it on a disk image with:"
echo "  make preseed   # only needed once per image, before its first leanfs-put - see the Makefile's own comment on this target"
echo "  build/leanfs-put build/os-image.bin $BUILD/$NAME.elf /bin/$NAME"
