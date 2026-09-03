#!/usr/bin/env bash
# tools/build-native-toolchain.sh - M98: the toolchain that runs HERE.
#
# Builds binutils (and, as the milestone advances, GCC and make) with
# --host=x86_64-lean_os: compiled BY the M94 cross compiler, runnable
# only ON lean_os. tools/install-native-toolchain.sh puts the result on
# the disk image.
#
# ---- what the first build of this actually hit --------------------------
#
# Configure succeeded outright; every stop was this libc, and the record
# is in milestones.md M98. The four header gaps came first, then
# <sys/param.h>, <memory.h>, PRId16, and a limits.h that GCC had been
# silently shadowing since M94 (fixed in build-toolchain.sh, where the
# comment names this script as the discoverer).
#
# ---- the two configure refusals, which are decisions --------------------
#
# --disable-plugins: bfd/plugin.c wants dlopen for LTO plugins. lean_os
#   HAS dlopen (M95) - in libc.so, not in the static libc the target's
#   specs link. No LTO plugin exists for this machine, so linking the
#   loader into a static toolchain to load nothing would be capability
#   theater. The day something concrete ships an LTO plugin here is the
#   day this flag comes off, and that day has a name in the deferred
#   list: it is the dynamic-linking condition, again.
#
# --disable-libctf: CTF is a debug format with zero producers and zero
#   consumers on this machine, and its library wants dlopen too. Same
#   argument, smaller stakes.
#
# The rest is M94's own recipe: MAKEINFO=true (no makeinfo here, no
# manuals wanted), --disable-nls (one language), and the gdb family off
# because binutils 2.43 ships gdb's tree alongside and nothing here has
# ported it.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

BINUTILS_VER=2.43
SRC="$ROOT/build/toolchain-src"
PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
NATIVE="$ROOT/build/native"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if [ ! -x "$PREFIX/bin/x86_64-lean_os-gcc" ]; then
  echo "build-native-toolchain: no cross compiler - run tools/build-toolchain.sh first" >&2
  exit 1
fi
[ -d "$SRC/binutils-$BINUTILS_VER" ] || {
  echo "build-native-toolchain: no binutils source - run tools/build-toolchain.sh first" >&2
  exit 1
}

# The sysroot is regenerated first for gcc-test.sh's reason: this build
# grades the libc as much as it grades anything, and it should grade the
# libc in the tree, not whatever a previous `make sysroot` left.
make -s -C "$ROOT" sysroot >/dev/null || exit 1

export PATH="$PREFIX/bin:$PATH"

mkdir -p "$NATIVE/binutils"
cd "$NATIVE/binutils"
if [ ! -f Makefile ]; then
  "$SRC/binutils-$BINUTILS_VER/configure" \
    --host=x86_64-lean_os --target=x86_64-lean_os --prefix=/usr \
    --disable-nls --disable-werror \
    --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng \
    --disable-plugins --disable-libctf \
    > configure.log 2>&1 || { tail -30 configure.log >&2; exit 1; }
fi
echo "build-native-toolchain: building binutils for x86_64-lean_os"
make -j"$JOBS" MAKEINFO=true > build.log 2>&1 || { tail -40 build.log >&2; exit 1; }

for t in gas/as-new ld/ld-new binutils/ar binutils/nm-new \
         binutils/objdump binutils/strip-new binutils/ranlib binutils/readelf; do
  [ -x "$t" ] || { echo "build-native-toolchain: $t did not build" >&2; exit 1; }
done

echo "build-native-toolchain: 8 tools built - tools/install-native-toolchain.sh puts them on the image"
