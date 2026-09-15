#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

LLVM_VER=19.1.7
TARGET=x86_64-lean_os

SRC="$ROOT/build/clang-src"
# M136 built the compiler from LLVM 24 - Chromium's own tree, unpacked at
# build/llvm24 - and left this default pointing at 19, so the two halves of
# one toolchain could be built from different sources without anybody saying
# so. M145 found out the way such things are found out: libc++ 19 defines its
# own isalpha_l when it is not told the C library has one, and this libc grew
# the _l family, and every C++ program stopped compiling. They take the same
# tree now.
DEFAULT_TREE="$ROOT/build/llvm24"
if [ ! -d "$DEFAULT_TREE" ]; then
  DEFAULT_TREE="$SRC/llvm-project-$LLVM_VER.src"
fi
TREE="${LEANOS_LLVM_TREE:-$DEFAULT_TREE}"
PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
SYSROOT="$ROOT/build/sysroot"
BUILDDIR="$ROOT/build/libcxx-build"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if [ ! -x "$PREFIX/bin/clang" ]; then
  echo "build-libcxx: no clang at $PREFIX - run tools/build-clang.sh first." >&2
  exit 1
fi
if [ ! -d "$TREE" ]; then
  echo "build-libcxx: no unpacked llvm-project at $TREE." >&2
  echo "              tools/build-clang.sh fetches and unpacks it." >&2
  exit 1
fi

# NOT `make sysroot`: its recipe begins with rm -rf and would take the fifteen
# third-party libraries installed under usr/lib with it, so building libc++
# would silently cost this tree its browser. The two halves M139 and M140 split
# out refresh the same headers and the same libc without deleting anything.
if [ -d "$SYSROOT/usr/include" ]; then
  make -s -C "$ROOT" sysroot-headers >/dev/null || exit 1
  make -s -C "$ROOT" sysroot-libc >/dev/null || exit 1
else
  make -s -C "$ROOT" sysroot >/dev/null || exit 1
fi

python3 "$ROOT/tools/clang-port/apply.py" "$TREE" >/dev/null || exit 1

SITE_DEFINES="_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE;_LIBCPP_HAS_NO_LIBRARY_ALIGNED_ALLOCATION"

# A cmake cache remembers which source tree it was configured against, and
# nothing else here would notice it pointing at a different one - which is how
# M145 rebuilt libc++ three times from the tree it was trying to move off.
CACHED_TREE=$(awk -F= '/^CMAKE_HOME_DIRECTORY:/{print $2}' \
              "$BUILDDIR/CMakeCache.txt" 2>/dev/null)
if [ -n "$CACHED_TREE" ] && [ "$CACHED_TREE" != "$TREE/runtimes" ]; then
  echo "build-libcxx: configured against $CACHED_TREE, want $TREE - reconfiguring"
  rm -rf "$BUILDDIR"
fi

if [ ! -f "$BUILDDIR/build.ninja" ]; then
  echo "build-libcxx: configuring"
  mkdir -p "$BUILDDIR"
  cmake -G Ninja -S "$TREE/runtimes" -B "$BUILDDIR" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/clang-port/cmake/lean_os.toolchain.cmake" \
    -DLEANOS_PREFIX="$PREFIX" \
    -DLEANOS_SYSROOT="$SYSROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX/$TARGET" \
    -DLLVM_ENABLE_RUNTIMES="libcxxabi;libcxx" \
    -DLIBCXX_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_STATIC=ON \
    -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBCXXABI_ENABLE_STATIC=ON \
    -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_ENABLE_EXCEPTIONS=ON \
    -DLIBCXX_ENABLE_RTTI=ON \
    -DLIBCXX_ENABLE_THREADS=ON \
    -DLIBCXX_HAS_PTHREAD_API=ON \
    -DLIBCXXABI_ENABLE_THREADS=ON \
    -DLIBCXXABI_HAS_PTHREAD_API=ON \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXXABI_USE_COMPILER_RT=OFF \
    -DLIBCXX_USE_COMPILER_RT=OFF \
    -DLIBCXX_HAS_MUSL_LIBC=OFF \
    -DLIBCXX_ENABLE_FILESYSTEM="${LEANOS_LIBCXX_FILESYSTEM:-ON}" \
    -DLIBCXX_ENABLE_LOCALIZATION="${LEANOS_LIBCXX_LOCALIZATION:-ON}" \
    -DLIBCXX_ENABLE_WIDE_CHARACTERS=ON \
    -DLIBCXX_ENABLE_RANDOM_DEVICE=ON \
    -DLIBCXX_ENABLE_UNICODE=ON \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF \
    -DLIBCXX_INCLUDE_DOCS=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBCXX_ENABLE_ASSERTIONS=OFF \
    -DLIBCXX_EXTRA_SITE_DEFINES="$SITE_DEFINES" \
    > "$ROOT/build/libcxx-cmake.log" 2>&1 || {
      tail -40 "$ROOT/build/libcxx-cmake.log" >&2; exit 1; }
fi

echo "build-libcxx: building with $JOBS jobs"
ninja -C "$BUILDDIR" -j "$JOBS" || exit 1

# The header directory is emptied first, because an install only ADDS files
# and a header the new version does not ship stays behind from the one that
# did. M145 moved this from libc++ 19 to 24 and libc++ 19's ctype.h survived
# in front of the C library's, which <cctype> in 24 notices and refuses to
# compile against - loudly, and only because it checks.
echo "build-libcxx: installing into $PREFIX/$TARGET"
rm -rf "$PREFIX/$TARGET/include/c++/v1"
ninja -C "$BUILDDIR" -j "$JOBS" install > /dev/null || exit 1

ls -l "$PREFIX/$TARGET/lib/libc++.a" "$PREFIX/$TARGET/lib/libc++abi.a" 2>/dev/null
echo "build-libcxx: done - tools/clang-test.sh grades it with tests/clang/cxx.cpp"
