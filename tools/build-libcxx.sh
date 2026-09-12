#!/usr/bin/env bash
# tools/build-libcxx.sh - M121: libc++ and libc++abi for x86_64-lean_os.
#
# The second half of docs/browser.md's third condition. Needs
# tools/build-clang.sh to have run - libc++ is built BY the clang this
# project ported, which is the only compiler that knows this target.
#
# ---- the three decisions in this file, and why ---------------------------
#
# 1. **Static only.** A shared libc++ would need a soname this loader
#    resolves and a versioned symbol table, and nothing on this machine
#    asks for one yet. M95's loader can do it; the day something needs it
#    is the day this changes.
#
# 2. **libc++abi over libgcc_eh, not over libunwind.** This is the one
#    real choice here. LLVM ships its own unwinder and libc++abi prefers
#    it; this build says no, because M94 and M97 already built GCC's and
#    every object in this sysroot - libc.a, the fifteen third-party
#    archives, crt1.o - was produced against it. Two unwinders in one
#    program is not a size problem, it is a correctness one: libgcc's
#    exception machinery keeps a static registry of the .eh_frame tables
#    it has been told about, and a throw that crosses from a frame one
#    unwinder registered into a frame the other did find no handler,
#    calls std::terminate, and aborts with no message. M97's note in
#    tools/toolchain-port/lean_os.h is the same argument, arrived at the
#    same way.
#
# 3. **Where it installs: $PREFIX/x86_64-lean_os, not the sysroot.**
#    CLAUDE.md names the hazard - the Makefile's `sysroot` target begins
#    `rm -rf $(SYSROOT)`, and tools/gcc-test.sh and tools/clang-test.sh
#    both run it. $PREFIX/<target> is where a GNU cross toolchain puts
#    target libraries anyway; it is where M97's libstdc++ already is.
#
# ---- and the one thing this libc cannot do, said rather than faked -----
#
# LIBCXX_ENABLE_ALIGNED_ALLOCATION=OFF. libc++ wants C11 `aligned_alloc`
# or POSIX `posix_memalign` for over-aligned `new` (`new (std::align_val_t)`),
# and **this libc has neither**. user_space/lib/malloc.c returns 16-byte
# aligned memory - which is max_align_t on x86-64, and which M115 had to
# fix after a `movaps` found the old 8 - but it cannot return more,
# because `free` locates a block by reading the header immediately before
# the pointer it was given and a shifted pointer has no header there.
#
# The condition for building it: a `free` that can find the block from a
# pointer that was moved forward, which means a tagged indirection header
# and a change to the allocator every program on this machine uses.
# Nothing asks for over-aligned new yet, and M65's rule decides the rest:
# an aligned_alloc that returned 16-byte memory for a request of 64 would
# be a function that pretends, and the program that believed it would
# fault somewhere else entirely.
#
# What is actually lost is only `new` with an explicit alignment.
# std::vector, std::string, every ordinary allocation and every type whose
# alignment is 16 or less are unaffected.
#
# Usage: tools/build-libcxx.sh
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

LLVM_VER=19.1.7
TARGET=x86_64-lean_os

SRC="$ROOT/build/clang-src"
TREE="$SRC/llvm-project-$LLVM_VER.src"
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

# The locale edit libc++ needs lives in the same port script as the
# compiler edits; re-applying is idempotent and costs nothing.
python3 "$ROOT/tools/clang-port/apply.py" "$TREE" >/dev/null || exit 1

# ---- the two facts about this platform that go INTO the headers --------
#
# Not compile flags: `__config_site` is installed beside the headers, so
# these reach every program that later includes <locale>, which is the
# only place they can be right. A -D on this build's own command line
# would configure the archive and leave every consumer disagreeing with
# it about the layout of ctype_base::mask - which is an ODR violation
# that links.
#
#   _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE
#     Which ctype table libc++ uses. Every branch of that switch in
#     <__locale> names a libc by macro - __GLIBC__, _NEWLIB_VERSION,
#     __APPLE__ - and the last one is `#error unknown rune table for
#     this platform`. This libc's <ctype.h> is functions rather than a
#     table (there is no `__ctype_b` here to point at), and this macro
#     is the supported answer to exactly that: libc++ compiles in its
#     OWN table, which src/locale.cpp holds in full and which is the C
#     locale's, correctly. It is what Bionic uses.
#
#   _LIBCPP_HAS_NO_LIBRARY_ALIGNED_ALLOCATION
#     See the note at the top of this file. This libc has neither
#     aligned_alloc nor posix_memalign, and this is the macro that says
#     so rather than the one that pretends.
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
