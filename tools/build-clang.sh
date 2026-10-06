#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

LLVM_VER=19.1.7
# The LLVM Chromium's own clang is built from, at the revision
# third_party/chromium/VENDORED pins: CLANG_REVISION in that checkout's
# tools/clang/scripts/update.py is llvmorg-24-init-7747-g62397f8b, and
# tools/chromium-test.sh requires the two to agree.
LLVM24_COMMIT=62397f8b3c3986f54187ce08f00b3448ea1f8880
TARGET=x86_64-lean_os

SRC="$ROOT/build/clang-src"
# M136 built the compiler from LLVM 24 - Chromium's own tree, unpacked at
# build/llvm24 - and left this default pointing at 19, so the two halves of
# one toolchain could be built from different sources without anybody saying
# so. M145 found out the way such things are found out: libc++ 19 defines its
# own isalpha_l when it is not told the C library has one, and this libc grew
# the _l family, and every C++ program stopped compiling. They take the same
# tree now.
#
# M226: and nothing said where build/llvm24 came from, so a second host fell
# back to 19 without a word and built a compiler Chromium's args.gn does not
# describe. It is fetched here now, at the commit above. LEANOS_LLVM=19 is
# M121's release tarball, which apply.py still applies to - the compiler to
# fall back on if a 24 build fails, never the default.
if [ "${LEANOS_LLVM:-24}" = "19" ]; then
  DEFAULT_TREE="$SRC/llvm-project-$LLVM_VER.src"
else
  DEFAULT_TREE="$ROOT/build/llvm24"
fi
TREE="${LEANOS_LLVM_TREE:-$DEFAULT_TREE}"
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
    echo "             Both are dev-time only." >&2
    exit 1
  }
done

echo "build-clang: generating the sysroot"
make -s -C "$ROOT" sysroot >/dev/null || exit 1

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
if [ -n "${LEANOS_LLVM_TREE:-}" ]; then
  echo "build-clang: using the tree at $TREE"
elif [ "$TREE" = "$ROOT/build/llvm24" ]; then
  if [ ! -d "$TREE/llvm" ]; then
    ARCHIVE="llvm-project-$LLVM24_COMMIT.tar.gz"
    if [ ! -f "$ARCHIVE" ]; then
      echo "build-clang: fetching llvm-project at ${LLVM24_COMMIT:0:12} (about 270 MB)"
      curl -sSL -o "$ARCHIVE.part" \
        "https://github.com/llvm/llvm-project/archive/$LLVM24_COMMIT.tar.gz" \
        && mv "$ARCHIVE.part" "$ARCHIVE" || exit 1
    fi
    echo "build-clang: unpacking it into $TREE"
    rm -rf "$TREE.part"
    mkdir -p "$TREE.part"
    tar xzf "$ARCHIVE" -C "$TREE.part" --strip-components=1 || exit 1
    mv "$TREE.part" "$TREE" || exit 1
  fi
  echo "build-clang: using the tree at $TREE"
elif [ ! -f "$TARBALL" ]; then
  echo "build-clang: fetching $TARBALL (about 135 MB)"
  curl -sSL -o "$TARBALL.part" \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$TARBALL" \
    && mv "$TARBALL.part" "$TARBALL" || exit 1
fi

FP=$(shasum -a 256 "$ROOT/tools/clang-port/apply.py" | cut -c1-16)
FP_FILE="$TREE/.lean_os-port-fingerprint"
# A cmake cache remembers the tree it was configured against and ninja would
# go on building that one - build-libcxx.sh learned this in M145.
CACHED_TREE=$(awk -F= '/^CMAKE_HOME_DIRECTORY:/{print $2}' \
              "$BUILDDIR/CMakeCache.txt" 2>/dev/null)
if [ -n "$CACHED_TREE" ] && [ "$CACHED_TREE" != "$TREE/llvm" ]; then
  echo "build-clang: configured against $CACHED_TREE, want $TREE - reconfiguring"
  rm -rf "$BUILDDIR"
fi
if [ -n "${LEANOS_LLVM_TREE:-}" ] || [ "$TREE" = "$ROOT/build/llvm24" ]; then
  if [ "$(cat "$FP_FILE" 2>/dev/null)" != "$FP" ]; then
    echo "build-clang: the port changed - reconfiguring against $TREE"
    rm -rf "$BUILDDIR"
  fi
else
  if [ -d "$TREE" ] && [ "$(cat "$FP_FILE" 2>/dev/null)" != "$FP" ]; then
    echo "build-clang: the port changed since this tree was unpacked - re-unpacking"
    rm -rf "$TREE" "$BUILDDIR"
  fi

  if [ ! -d "$TREE" ]; then
    echo "build-clang: unpacking $TARBALL"
    tar xf "$TARBALL" || exit 1
  fi
fi

echo "build-clang: applying tools/clang-port"
python3 "$ROOT/tools/clang-port/apply.py" "$TREE" || exit 1
echo "$FP" > "$FP_FILE"

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

for t in clang clang++; do
  ln -sf clang "$PREFIX/bin/$TARGET-$t"
done
ln -sf clang "$PREFIX/bin/$TARGET-cc"
ln -sf clang "$PREFIX/bin/$TARGET-c++"

echo "build-clang: done"
"$PREFIX/bin/$TARGET-clang" --version | head -2

if [ "${1:-}" = "--libcxx" ]; then
  exec "$ROOT/tools/build-libcxx.sh"
fi
