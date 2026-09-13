#!/usr/bin/env bash
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
