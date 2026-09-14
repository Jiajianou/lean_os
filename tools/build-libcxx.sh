#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

LLVM_VER=19.1.7
TARGET=x86_64-lean_os

SRC="$ROOT/build/clang-src"
TREE="${LEANOS_LLVM_TREE:-$SRC/llvm-project-$LLVM_VER.src}"
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

make -s -C "$ROOT" sysroot >/dev/null || exit 1

python3 "$ROOT/tools/clang-port/apply.py" "$TREE" >/dev/null || exit 1

SITE_DEFINES="_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE;_LIBCPP_HAS_NO_LIBRARY_ALIGNED_ALLOCATION"

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
    -DLIBCXX_ENABLE_FILESYSTEM="${LEANOS_LIBCXX_FILESYSTEM:-OFF}" \
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

echo "build-libcxx: installing into $PREFIX/$TARGET"
ninja -C "$BUILDDIR" -j "$JOBS" install > /dev/null || exit 1

ls -l "$PREFIX/$TARGET/lib/libc++.a" "$PREFIX/$TARGET/lib/libc++abi.a" 2>/dev/null
echo "build-libcxx: done - tools/clang-test.sh grades it with tests/clang/cxx.cpp"
