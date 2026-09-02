#!/usr/bin/env bash
# tools/build-toolchain.sh - M94: a compiler that knows this OS by name.
#
# Builds `x86_64-lean_os-gcc` and its binutils against the sysroot `make
# sysroot` produces, so that
#
#     x86_64-lean_os-gcc hello.c -o hello
#
# produces a program this machine runs, with **no flag invented by
# hand** - which is the entire point of the milestone, because every flag
# invented by hand is a flag someone else's build system will not pass.
#
# ---- why this is not `brew install` -------------------------------------
#
# docs/toolchain.md's table is dev-time tools this project uses. This is
# a compiler *for* this project's target, and no distribution has one
# because the target did not exist until M94 named it. The port itself is
# tools/toolchain-port/, which is nine anchored edits and one header -
# see that directory's apply.py for why it is a script of edits rather
# than a patch series, and for the line M94 draws between configuring a
# target and forking a compiler.
#
# ---- what it does NOT build ---------------------------------------------
#
# A cross compiler only: it runs on the machine you are sitting at and
# emits code for lean_os. GCC running *on* lean_os is M98 and needs this
# one first. No libstdc++ (M97), no shared libraries (M95) - the specs
# say -static and lean_os.h says why.
#
# Usage:
#   tools/build-toolchain.sh          # fetch, port, configure, build, install
#   tools/build-toolchain.sh --check  # just say whether it is already there
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

BINUTILS_VER=2.43
GCC_VER=14.2.0
TARGET=x86_64-lean_os

SRC="$ROOT/build/toolchain-src"
PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
SYSROOT="$ROOT/build/sysroot"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if [ "${1:-}" = "--check" ]; then
  if [ -x "$PREFIX/bin/$TARGET-gcc" ]; then
    echo "build-toolchain: $PREFIX/bin/$TARGET-gcc is present"
    "$PREFIX/bin/$TARGET-gcc" --version | head -1
    exit 0
  fi
  echo "build-toolchain: no $TARGET-gcc at $PREFIX - run this script" >&2
  exit 1
fi

# The sysroot has to exist BEFORE gcc is configured: libgcc is compiled
# against it, and a libgcc built against no headers is a libgcc that
# cannot use one later.
echo "build-toolchain: generating the sysroot"
make -s -C "$ROOT" sysroot || exit 1

mkdir -p "$SRC"
cd "$SRC"

fetch() {
  local url=$1 file=$2
  [ -f "$file" ] && return 0
  echo "build-toolchain: fetching $file"
  curl -sSL -o "$file.part" "$url" && mv "$file.part" "$file"
}

fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz" \
      "binutils-$BINUTILS_VER.tar.xz"
fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz" \
      "gcc-$GCC_VER.tar.xz"

[ -d "binutils-$BINUTILS_VER" ] || tar xf "binutils-$BINUTILS_VER.tar.xz"
[ -d "gcc-$GCC_VER" ] || tar xf "gcc-$GCC_VER.tar.xz"

echo "build-toolchain: applying the target port"
python3 "$ROOT/tools/toolchain-port/apply.py" \
        "$SRC/binutils-$BINUTILS_VER" "$SRC/gcc-$GCC_VER" || exit 1

# GCC needs gmp/mpfr/mpc. Homebrew's are what this machine has; pointed
# at explicitly rather than left to configure's search, because a
# configure that finds a different one than the person expected is a
# build that fails an hour later.
GMP="$(brew --prefix gmp 2>/dev/null || echo /usr/local)"
MPFR="$(brew --prefix mpfr 2>/dev/null || echo /usr/local)"
MPC="$(brew --prefix libmpc 2>/dev/null || echo /usr/local)"

mkdir -p "$SRC/build-binutils" "$SRC/build-gcc"

echo "build-toolchain: configuring binutils ($JOBS jobs)"
cd "$SRC/build-binutils"
if [ ! -f Makefile ]; then
  "../binutils-$BINUTILS_VER/configure" \
    --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
    --disable-nls --disable-werror --enable-lto \
    --with-system-zlib \
    > configure.log 2>&1 || { tail -30 configure.log >&2; exit 1; }
  # --with-system-zlib, because the bundled one does not compile against
  # a current macOS SDK: zlib's zutil.c uses K&R definitions that clash
  # with the SDK's own <_stdio.h> renaming, and the first error names a
  # header nobody here wrote. The host has a zlib; using it is both the
  # fix and the smaller thing.
fi
# MAKEINFO=true, because `makeinfo` is not on this machine and the info
# manuals are not what is being built. binutils' own build treats a
# missing makeinfo as a hard error (exit 127 from a doc rule), which
# stops a compiler build over documentation nobody asked for. `true` is
# the substitution its own maintainers document for exactly this.
echo "build-toolchain: building binutils"
make -j"$JOBS" MAKEINFO=true > build.log 2>&1 || { tail -40 build.log >&2; exit 1; }
make install MAKEINFO=true > install.log 2>&1 || { tail -20 install.log >&2; exit 1; }

export PATH="$PREFIX/bin:$PATH"

echo "build-toolchain: configuring gcc"
cd "$SRC/build-gcc"
if [ ! -f Makefile ]; then
  "../gcc-$GCC_VER/configure" \
    --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
    --enable-languages=c --disable-nls --disable-werror \
    --disable-shared --disable-libssp --disable-libquadmath \
    --disable-libgomp --disable-libatomic --disable-multilib \
    --enable-initfini-array \
    --with-gmp="$GMP" --with-mpfr="$MPFR" --with-mpc="$MPC" \
    --with-system-zlib \
    > configure.log 2>&1 || { tail -40 configure.log >&2; exit 1; }
fi
echo "build-toolchain: building gcc (this is the long one)"
make -j"$JOBS" MAKEINFO=true all-gcc > build-gcc.log 2>&1 || { tail -40 build-gcc.log >&2; exit 1; }
make -j"$JOBS" MAKEINFO=true all-target-libgcc > build-libgcc.log 2>&1 || { tail -40 build-libgcc.log >&2; exit 1; }
make MAKEINFO=true install-gcc > install-gcc.log 2>&1 || { tail -20 install-gcc.log >&2; exit 1; }
make MAKEINFO=true install-target-libgcc > install-libgcc.log 2>&1 || { tail -20 install-libgcc.log >&2; exit 1; }

echo "build-toolchain: done"
"$PREFIX/bin/$TARGET-gcc" --version | head -1
echo "build-toolchain: add $PREFIX/bin to PATH"
