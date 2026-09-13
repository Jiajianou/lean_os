#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

NATIVE=build/native/binutils
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT=build/leanfs-put
CROSS_STRIP=build/toolchain/bin/x86_64-lean_os-strip

if [ ! -x "$NATIVE/gas/as-new" ]; then
  echo "install-native-toolchain: no native binutils at $NATIVE - skipped."
  echo "          tools/build-native-toolchain.sh builds it (M98); it is not part of \`make\`."
  exit 0
fi
[ -f "$IMAGE" ] || { echo "install-native-toolchain: no image at $IMAGE - run make first" >&2; exit 1; }
[ -x "$PUT" ] || make -s leanfs-put || exit 1

STAGE=build/native/install
mkdir -p "$STAGE"

install_one() {
  local from=$1 name=$2
  cp "$from" "$STAGE/$name" || exit 1
  "$CROSS_STRIP" "$STAGE/$name" || exit 1
  "$PUT" "$IMAGE" "$STAGE/$name" "/bin/$name" >/dev/null || exit 1
}

install_one "$NATIVE/gas/as-new"         as
install_one "$NATIVE/ld/ld-new"          ld
install_one "$NATIVE/binutils/ar"        ar
install_one "$NATIVE/binutils/nm-new"    nm
install_one "$NATIVE/binutils/objdump"   objdump
install_one "$NATIVE/binutils/strip-new" strip
install_one "$NATIVE/binutils/ranlib"    ranlib
install_one "$NATIVE/binutils/readelf"   readelf

"$PUT" "$IMAGE" tests/binutils/hello.s /tests/binutils-hello.s >/dev/null || exit 1

TOTAL=$(du -sh "$STAGE" | cut -f1)
echo "install-native-toolchain: 8 binutils tools in /bin ($TOTAL stripped)"

GCCSTAGE=build/native/gcc-install
CROSS=build/toolchain
if [ ! -x "$GCCSTAGE/usr/libexec/gcc/x86_64-lean_os/14.2.0/cc1" ]; then
  echo "install-native-toolchain: no native gcc staged - binutils only."
  exit 0
fi

GCCDIR=/usr/lib/gcc/x86_64-lean_os/14.2.0
EXECDIR=/usr/libexec/gcc/x86_64-lean_os/14.2.0

strip_put() {
  local from=$1 to=$2 base
  base=$(basename "$from")
  cp "$from" "$STAGE/$base" || exit 1
  "$CROSS_STRIP" "$STAGE/$base" 2>/dev/null || true
  "$PUT" "$IMAGE" "$STAGE/$base" "$to" >/dev/null || exit 1
}

strip_put "$GCCSTAGE/usr/bin/gcc"      /usr/bin/gcc
strip_put "$GCCSTAGE/usr/bin/g++"      /usr/bin/g++
strip_put "$GCCSTAGE/usr/bin/cpp"      /usr/bin/cpp
strip_put "$GCCSTAGE/$EXECDIR/cc1"      $EXECDIR/cc1
strip_put "$GCCSTAGE/$EXECDIR/cc1plus"  $EXECDIR/cc1plus
strip_put "$GCCSTAGE/$EXECDIR/collect2" $EXECDIR/collect2

"$PUT" -r "$IMAGE" "$GCCSTAGE$GCCDIR/include" $GCCDIR/include >/dev/null || exit 1

for f in crtbegin.o crtend.o crtbeginS.o crtendS.o libgcc.a libgcc_eh.a; do
  "$PUT" "$IMAGE" "$CROSS/lib/gcc/x86_64-lean_os/14.2.0/$f" $GCCDIR/$f >/dev/null || exit 1
done

"$PUT" -r "$IMAGE" build/sysroot/usr/include       /usr/include       >/dev/null || exit 1
"$PUT" -r "$IMAGE" build/sysroot/usr/local/include /usr/local/include >/dev/null || exit 1
for f in libc.a libm.a crt1.o crti.o crtn.o Scrt1.o lean_os.ld; do
  "$PUT" "$IMAGE" "build/sysroot/usr/lib/$f" /usr/lib/$f >/dev/null || exit 1
done

"$PUT" -r "$IMAGE" "$CROSS/x86_64-lean_os/include/c++" /usr/include/c++ >/dev/null || exit 1
for f in libstdc++.a libsupc++.a; do
  "$PUT" "$IMAGE" "$CROSS/x86_64-lean_os/lib/$f" /usr/lib/$f >/dev/null || exit 1
done

if [ -x build/native/make/make ]; then
  strip_put build/native/make/make /usr/bin/make
fi

"$PUT" "$IMAGE" tests/binutils/m98c.c /tests/m98c.c >/dev/null || exit 1
"$PUT" -r "$IMAGE" tests/binutils/m98mk /tests/m98mk >/dev/null || exit 1

"$PUT" -r "$IMAGE" tests/bootstrap /tests/bootstrap >/dev/null || exit 1
"$PUT" -r "$IMAGE" tests/pybuild /tests/pybuild >/dev/null || exit 1

BZSRC=build/thirdparty-src/bzip2-1.0.8
if [ -d "$BZSRC" ]; then
  BZSTAGE=build/native/bzip2-src
  BZREF=build/native/bzip2-ref
  rm -rf "$BZSTAGE" "$BZREF"
  mkdir -p "$BZSTAGE"
  for f in "$BZSRC"/*.c "$BZSRC"/*.h "$BZSRC"/Makefile \
           "$BZSRC"/sample*.ref "$BZSRC"/sample*.bz2 "$BZSRC"/words*; do
    [ -f "$f" ] && cp "$f" "$BZSTAGE/"
  done
  cp -R "$BZSTAGE" "$BZREF"
  BZCFLAGS="-Wall -Winline -O2 -D_FILE_OFFSET_BITS=64"
  ( cd "$BZREF" && make -s CC="$ROOT/$CROSS/bin/x86_64-lean_os-gcc" \
      AR="$ROOT/$CROSS/bin/x86_64-lean_os-ar" \
      RANLIB="$ROOT/$CROSS/bin/x86_64-lean_os-ranlib" \
      CFLAGS="$BZCFLAGS" libbz2.a bzip2 bzip2recover \
      > cross-build.log 2>&1 ) || {
    echo "install-native-toolchain: the cross reference build of bzip2 failed:" >&2
    tail -20 "$BZREF/cross-build.log" >&2
    exit 1
  }
  "$PUT" -r "$IMAGE" "$BZSTAGE" /tests/bzip2 >/dev/null || exit 1
  mkdir -p build/native/bzip2-ref-objs
  for o in "$BZREF"/*.o; do
    cp "$o" build/native/bzip2-ref-objs/
  done
  "$PUT" -r "$IMAGE" build/native/bzip2-ref-objs /tests/bzip2-ref >/dev/null || exit 1
  echo "install-native-toolchain: bzip2's source in /tests/bzip2, and the cross compiler's own objects beside it"
else
  echo "install-native-toolchain: no bzip2 source at $BZSRC - the bootstrap boot will skip the build half."
  echo "          tools/build-thirdparty.sh fetches it."
fi

echo "install-native-toolchain: gcc, g++, cpp, make, headers and libraries under /usr"
