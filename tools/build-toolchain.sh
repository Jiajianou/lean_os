#!/usr/bin/env bash
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

PORT_STAMP=$(cat "$ROOT"/tools/toolchain-port/apply.py | shasum -a 256 | cut -d' ' -f1)
for d in "binutils-$BINUTILS_VER" "gcc-$GCC_VER"; do
  if [ -d "$d" ] && [ "$(cat "$d/.lean_os-port-stamp" 2>/dev/null)" != "$PORT_STAMP" ]; then
    echo "build-toolchain: the target port changed - unpacking a clean $d"
    rm -rf "$d" "$SRC/build-${d%%-*}"
  fi
done

[ -d "binutils-$BINUTILS_VER" ] || tar xf "binutils-$BINUTILS_VER.tar.xz"
[ -d "gcc-$GCC_VER" ] || tar xf "gcc-$GCC_VER.tar.xz"

echo "build-toolchain: applying the target port"
python3 "$ROOT/tools/toolchain-port/apply.py" \
        "$SRC/binutils-$BINUTILS_VER" "$SRC/gcc-$GCC_VER" || exit 1
echo "$PORT_STAMP" > "$SRC/binutils-$BINUTILS_VER/.lean_os-port-stamp"
echo "$PORT_STAMP" > "$SRC/gcc-$GCC_VER/.lean_os-port-stamp"

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
fi
echo "build-toolchain: building binutils"
make -j"$JOBS" MAKEINFO=true > build.log 2>&1 || { tail -40 build.log >&2; exit 1; }
make install MAKEINFO=true > install.log 2>&1 || { tail -20 install.log >&2; exit 1; }

export PATH="$PREFIX/bin:$PATH"

echo "build-toolchain: configuring gcc"
cd "$SRC/build-gcc"
if [ ! -f Makefile ]; then
  "../gcc-$GCC_VER/configure" \
    --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
    --enable-languages=c,c++ --disable-nls --disable-werror \
    --enable-shared --disable-libssp --disable-libquadmath \
    --disable-libgomp --disable-libatomic --disable-multilib \
    --enable-initfini-array \
    --disable-libstdcxx-verbose --enable-threads=posix \
    --with-gmp="$GMP" --with-mpfr="$MPFR" --with-mpc="$MPC" \
    --with-system-zlib \
    > configure.log 2>&1 || { tail -40 configure.log >&2; exit 1; }
fi
echo "build-toolchain: building gcc (this is the long one)"
make -j"$JOBS" MAKEINFO=true LIMITS_H_TEST=true all-gcc > build-gcc.log 2>&1 || { tail -40 build-gcc.log >&2; exit 1; }
make -j"$JOBS" MAKEINFO=true all-target-libgcc > build-libgcc.log 2>&1 || { tail -40 build-libgcc.log >&2; exit 1; }
make MAKEINFO=true LIMITS_H_TEST=true install-gcc > install-gcc.log 2>&1 || { tail -20 install-gcc.log >&2; exit 1; }
make MAKEINFO=true install-target-libgcc > install-libgcc.log 2>&1 || { tail -20 install-libgcc.log >&2; exit 1; }

echo "build-toolchain: building the C++ runtime (libsupc++)"
ABI_STAMP=$(cat "$ROOT"/user_space/libc/include/*.h "$ROOT"/user_space/libc/include/*/*.h \
                "$ROOT"/system_api/include/*.h 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
if [ -d "$TARGET/libstdc++-v3" ] && \
   [ "$(cat "$TARGET/libstdc++-v3/.lean_os-abi-stamp" 2>/dev/null)" != "$ABI_STAMP" ]; then
  echo "build-toolchain: the libc headers changed since libstdc++ was built - reconfiguring it"
  rm -rf "$TARGET/libstdc++-v3"
fi
make -j"$JOBS" MAKEINFO=true all-target-libstdc++-v3 > build-cxx.log 2>&1 \
  || { tail -40 build-cxx.log >&2; exit 1; }
echo "$ABI_STAMP" > "$TARGET/libstdc++-v3/.lean_os-abi-stamp"
make MAKEINFO=true install-target-libstdc++-v3 > install-cxx.log 2>&1 \
  || { tail -20 install-cxx.log >&2; exit 1; }

echo "build-toolchain: done"
"$PREFIX/bin/$TARGET-gcc" --version | head -1
echo "build-toolchain: add $PREFIX/bin to PATH"
