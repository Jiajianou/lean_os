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
echo "build-native-toolchain: binutils - 8 tools"

# ---- GCC's three arithmetic libraries, built for the target -----------
#
# The cross gcc used brew's copies because it RUNS on the Mac; this one
# runs on lean_os, so all three are compiled for it, static, into one
# staging prefix the gcc configure below is pointed at. Each tarball is
# fetched by this script the same way build-toolchain.sh fetches its
# two, and each config.sub learns the OS name through apply.py's one
# anchored edit - gmp's lives in configfsf.sub, wrapped by a config.sub
# of gmp's own that only rewrites the CPU half.
#
# What building them taught (M98's notes have the long form): gmp.h
# declares its FILE* functions only if it can SEE that <stdio.h> was
# included, by testing thirteen libcs' guard macros - so <stdio.h> now
# defines _STDIO_H beside its #pragma once - and gmp's printf wants
# XPG's isascii, which <ctype.h> now has.
GMP_VER=6.3.0
MPFR_VER=4.2.1
MPC_VER=1.3.1
NPREFIX="$NATIVE/prefix"

cd "$SRC"
fetch() {
  local url=$1 file=$2
  [ -f "$file" ] && return 0
  echo "build-native-toolchain: fetching $file"
  curl -sSL -o "$file.part" "$url" && mv "$file.part" "$file"
}
fetch "https://ftp.gnu.org/gnu/gmp/gmp-$GMP_VER.tar.xz" "gmp-$GMP_VER.tar.xz"
fetch "https://ftp.gnu.org/gnu/mpfr/mpfr-$MPFR_VER.tar.xz" "mpfr-$MPFR_VER.tar.xz"
fetch "https://ftp.gnu.org/gnu/mpc/mpc-$MPC_VER.tar.gz" "mpc-$MPC_VER.tar.gz"
[ -d "gmp-$GMP_VER" ] || tar xf "gmp-$GMP_VER.tar.xz"
[ -d "mpfr-$MPFR_VER" ] || tar xf "mpfr-$MPFR_VER.tar.xz"
[ -d "mpc-$MPC_VER" ] || tar xf "mpc-$MPC_VER.tar.gz"

for cs in "gmp-$GMP_VER/configfsf.sub" "mpfr-$MPFR_VER/config.sub" \
          "mpc-$MPC_VER/build-aux/config.sub"; do
  python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub "$SRC/$cs" \
    > /dev/null || exit 1
done

build_dep() {
  local name=$1 srcdir=$2; shift 2
  mkdir -p "$NATIVE/$name"
  cd "$NATIVE/$name"
  if [ ! -f Makefile ]; then
    "$SRC/$srcdir/configure" --host=x86_64-lean_os --prefix="$NPREFIX" \
      --disable-shared --enable-static "$@" \
      > configure.log 2>&1 || { tail -20 configure.log >&2; exit 1; }
  fi
  echo "build-native-toolchain: $name"
  make -j"$JOBS" > build.log 2>&1 || { tail -30 build.log >&2; exit 1; }
  make install > install.log 2>&1 || { tail -10 install.log >&2; exit 1; }
}

build_dep gmp  "gmp-$GMP_VER"
build_dep mpfr "mpfr-$MPFR_VER" --with-gmp="$NPREFIX"
build_dep mpc  "mpc-$MPC_VER"   --with-gmp="$NPREFIX" --with-mpfr="$NPREFIX"

# ---- GCC itself, and make ----------------------------------------------
#
# --with-sysroot=/ because ON THE MACHINE the headers and libraries are
# at /usr; --with-build-sysroot points fixincludes (which runs on the
# Mac) at the Mac-side copy, or it looks for //usr/include and dies.
# LIMITS_H_TEST=true for the reason build-toolchain.sh documents - the
# native compiler must not repeat M94's shadowed-limits.h bug.
#
# A WARNING THE HARD WAY: configure decides HAVE_REALPATH and kin at
# configure time. A libc function added after a tree was configured
# does not exist for that tree until it is reconfigured from scratch -
# lrealpath quietly fell into undefined behaviour over exactly this,
# and the "input file is the same as output file" wall it produced
# cost three diagnostic boots to trace. When the libc grows, rm -rf
# the build directories; this script always starts clean enough
# because Makefile-existence is its only cache.
GCC_VER=14.2.0
mkdir -p "$NATIVE/gcc"
cd "$NATIVE/gcc"
if [ ! -f Makefile ]; then
  "$SRC/gcc-$GCC_VER/configure" \
    --host=x86_64-lean_os --target=x86_64-lean_os --prefix=/usr \
    --with-sysroot=/ --with-build-sysroot="$ROOT/build/sysroot" \
    --enable-languages=c,c++ --disable-nls --disable-werror \
    --disable-shared --disable-libssp --disable-libquadmath \
    --disable-libgomp --disable-libatomic --disable-multilib \
    --enable-initfini-array --disable-libstdcxx-verbose \
    --enable-threads=posix \
    --with-gmp="$NPREFIX" --with-mpfr="$NPREFIX" --with-mpc="$NPREFIX" \
    > configure.log 2>&1 || { tail -30 configure.log >&2; exit 1; }
fi
echo "build-native-toolchain: gcc (this is the long one)"
make -j"$JOBS" MAKEINFO=true LIMITS_H_TEST=true all-gcc \
  > build.log 2>&1 || { tail -40 build.log >&2; exit 1; }
make -s MAKEINFO=true LIMITS_H_TEST=true \
  DESTDIR="$NATIVE/gcc-install" install-gcc \
  > install.log 2>&1 || { tail -20 install.log >&2; exit 1; }

# GNU make: --disable-load is the same refusal binutils'
# --disable-plugins is (its loadable-object feature wants dlopen).
MAKE_VER=4.4.1
cd "$SRC"
fetch "https://ftp.gnu.org/gnu/make/make-$MAKE_VER.tar.gz" "make-$MAKE_VER.tar.gz"
[ -d "make-$MAKE_VER" ] || tar xf "make-$MAKE_VER.tar.gz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
  "$SRC/make-$MAKE_VER/build-aux/config.sub" > /dev/null || exit 1
mkdir -p "$NATIVE/make"
cd "$NATIVE/make"
if [ ! -f Makefile ]; then
  "$SRC/make-$MAKE_VER/configure" --host=x86_64-lean_os --prefix=/usr \
    --disable-load > configure.log 2>&1 || { tail -20 configure.log >&2; exit 1; }
fi
echo "build-native-toolchain: make"
make -j"$JOBS" > build.log 2>&1 || { tail -30 build.log >&2; exit 1; }

echo "build-native-toolchain: binutils, gmp/mpfr/mpc, gcc and make built - tools/install-native-toolchain.sh puts them on the image"
