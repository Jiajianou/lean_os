#!/usr/bin/env bash
# tools/build-clang.sh - M121: a second compiler that knows this OS by name.
#
# Builds `x86_64-lean_os-clang` (and clang++) against the sysroot `make
# sysroot` produces, so that
#
#     x86_64-lean_os-clang hello.c -o hello
#
# produces a program this machine runs, with **no flag invented by hand** -
# the same standard M94 set for GCC and for the same reason: every flag
# invented by hand is a flag someone else's build system will not pass.
#
# ---- why a second compiler at all ---------------------------------------
#
# docs/browser.md's measurement of Chromium ends in five conditions, and
# the third is "clang and libc++ for x86_64-lean_os": Chromium supports no
# other toolchain, and neither does any other modern engine's build. This
# is that condition. M94's nine-edit GCC port is the template for how much
# it costs; tools/clang-port/apply.py is the answer - six edits and two
# files of this project's own.
#
# ---- where it installs, and why it is the GCC prefix --------------------
#
# $PREFIX is build/toolchain, the same prefix tools/build-toolchain.sh
# uses, and that is load-bearing rather than tidy. clang finds three
# things there with no configuration:
#
#   x86_64-lean_os-ld       ToolChain's constructor puts the driver's own
#                           directory on the program paths.
#   crtbegin.o, crtend.o    GCCInstallationDetector searches
#   libgcc.a, libgcc_eh.a   `<driver dir>/..` for lib/gcc/<triple>/<ver>.
#
# One target, one prefix, two front ends. The alternative is
# --gcc-toolchain= on every command line, which is exactly the kind of
# flag this milestone exists to not need.
#
# Usage:
#   tools/build-clang.sh            # fetch, port, configure, build, install
#   tools/build-clang.sh --check    # just say whether it is already there
#   tools/build-clang.sh --libcxx   # ...and build libc++/libc++abi for the target
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

LLVM_VER=19.1.7
TARGET=x86_64-lean_os

SRC="$ROOT/build/clang-src"
TREE="$SRC/llvm-project-$LLVM_VER.src"
PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
SYSROOT="$ROOT/build/sysroot"
BUILDDIR="$ROOT/build/clang-build"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if [ "${1:-}" = "--check" ]; then
  if [ -x "$PREFIX/bin/$TARGET-clang" ]; then
    echo "build-clang: $PREFIX/bin/$TARGET-clang is present"
    "$PREFIX/bin/$TARGET-clang" --version | head -2
    exit 0
  fi
  echo "build-clang: no $TARGET-clang at $PREFIX - run this script" >&2
  exit 1
fi

for tool in cmake ninja; do
  command -v "$tool" >/dev/null || {
    echo "build-clang: $tool is required and is not on PATH." >&2
    echo "             Both are dev-time only - see docs/toolchain.md." >&2
    exit 1
  }
done

# The sysroot has to exist BEFORE clang is configured, for the same reason
# it does for GCC: DEFAULT_SYSROOT is baked into the binary, and libc++
# is compiled against the headers in it.
echo "build-clang: generating the sysroot"
make -s -C "$ROOT" sysroot >/dev/null || exit 1

# And the GCC half has to be there, because clang links against it. This
# is a hard requirement rather than a preference: crtbegin.o and
# libgcc_eh.a are M94's and M97's and there is no second copy.
if [ ! -x "$PREFIX/bin/$TARGET-ld" ]; then
  echo "build-clang: no $TARGET-ld at $PREFIX." >&2
  echo "             tools/build-toolchain.sh builds the binutils and GCC" >&2
  echo "             half first; clang links against its libgcc and uses" >&2
  echo "             its ld. See the note at the top of this file." >&2
  exit 1
fi

mkdir -p "$SRC"
cd "$SRC"

TARBALL="llvm-project-$LLVM_VER.src.tar.xz"
if [ ! -f "$TARBALL" ]; then
  echo "build-clang: fetching $TARBALL (about 135 MB)"
  curl -sSL -o "$TARBALL.part" \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$TARBALL" \
    && mv "$TARBALL.part" "$TARBALL" || exit 1
fi

# ---- the port's own fingerprint -----------------------------------------
#
# The same hazard M99 hit with the GCC port, and the same answer.
# tools/clang-port/apply.py is a set of ANCHORED EDITS, and an anchored
# edit is idempotent only while the edit itself does not change: it
# re-applies when it cannot find its own replacement text, and it finds
# its anchor in the pristine text either side of the block it added last
# time. So changing an edit adds a SECOND copy of it and leaves the first
# - which, in a C++ enum or a `case` list, is a compile error at best and
# a silently-shadowed first definition at worst.
#
# The fix is not to make the edits cleverer. It is to throw the unpacked
# tree away whenever the port changes, and a hash of the port says when.
#
# **Only apply.py is in the hash**, and that is the distinction rather
# than an oversight: LeanOS.h and LeanOS.cpp are COPIED over whatever is
# there, so a change to either is idempotent by construction and costs one
# file's recompile. Hashing them too would throw away a 2,600-target build
# for an edit that cannot double-apply, which is how this was found - and
# the general rule is worth writing down: an anchored edit needs the
# reset, a whole file does not.
FP=$(shasum -a 256 tools/clang-port/apply.py | cut -c1-16)
FP_FILE="$TREE/.lean_os-port-fingerprint"
if [ -d "$TREE" ] && [ "$(cat "$FP_FILE" 2>/dev/null)" != "$FP" ]; then
  echo "build-clang: the port changed since this tree was unpacked - re-unpacking"
  rm -rf "$TREE" "$BUILDDIR"
fi

if [ ! -d "$TREE" ]; then
  echo "build-clang: unpacking $TARBALL"
  tar xf "$TARBALL" || exit 1
fi

echo "build-clang: applying tools/clang-port"
python3 "$ROOT/tools/clang-port/apply.py" "$TREE" || exit 1
echo "$FP" > "$FP_FILE"

# ---- configure -----------------------------------------------------------
#
# LLVM_DEFAULT_TARGET_TRIPLE is what makes this a cross compiler rather
# than a host clang with a flag: `clang hello.c` targets lean_os, the same
# way `x86_64-lean_os-gcc hello.c` does, and nothing has to pass --target.
#
# X86 alone for LLVM_TARGETS_TO_BUILD: this OS is x86-64 only (the legacy
# BIOS path went in M26 and no other architecture has ever been on the
# table), and the other seventeen back ends are most of the build time.
if [ ! -f "$BUILDDIR/build.ninja" ]; then
  echo "build-clang: configuring (cmake + ninja)"
  mkdir -p "$BUILDDIR"
  cmake -G Ninja -S "$TREE/llvm" -B "$BUILDDIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_DEFAULT_TARGET_TRIPLE="$TARGET" \
    -DDEFAULT_SYSROOT="$SYSROOT" \
    -DCLANG_DEFAULT_CXX_STDLIB=libc++ \
    -DCLANG_DEFAULT_RTLIB=libgcc \
    -DCLANG_DEFAULT_UNWINDLIB=none \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_APPEND_VC_REV=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
    -DCLANG_ENABLE_ARCMT=OFF \
    > "$ROOT/build/clang-cmake.log" 2>&1 || {
      tail -30 "$ROOT/build/clang-cmake.log" >&2; exit 1; }
fi

echo "build-clang: building clang with $JOBS jobs (this is the long part)"
ninja -C "$BUILDDIR" -j "$JOBS" clang || exit 1

echo "build-clang: installing into $PREFIX"
ninja -C "$BUILDDIR" -j "$JOBS" install-clang install-clang-resource-headers \
      install-llvm-headers >/dev/null 2>&1 || \
  ninja -C "$BUILDDIR" -j "$JOBS" install-clang install-clang-resource-headers \
  || exit 1

# ---- the cross-compiler names --------------------------------------------
#
# clang reads its own argv[0] for a target prefix, so these two links are
# not cosmetic: they are what makes `x86_64-lean_os-clang` mean the same
# thing as `x86_64-lean_os-gcc` to a build system that derives tool names
# from a triple - and ./configure --host=x86_64-lean_os is exactly such a
# build system. The default triple above already makes the un-prefixed
# binary target lean_os; this makes the prefixed name work too.
for t in clang clang++; do
  ln -sf clang "$PREFIX/bin/$TARGET-$t"
done
# cc/c++ are what some hand-written Makefiles look for.
ln -sf clang "$PREFIX/bin/$TARGET-cc"
ln -sf clang "$PREFIX/bin/$TARGET-c++"

echo "build-clang: done"
"$PREFIX/bin/$TARGET-clang" --version | head -2

# ---- libc++ and libc++abi, on request -----------------------------------
if [ "${1:-}" = "--libcxx" ]; then
  exec "$ROOT/tools/build-libcxx.sh"
fi
