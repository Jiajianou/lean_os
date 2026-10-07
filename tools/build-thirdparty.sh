#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
if [ ! -x "$PREFIX/bin/x86_64-lean_os-gcc" ]; then
  echo "build-thirdparty: no x86_64-lean_os-gcc - run tools/build-toolchain.sh" >&2
  exit 1
fi
export PATH="$PREFIX/bin:$PATH"

make -s -C "$ROOT" sysroot >/dev/null || exit 1

SRC="$ROOT/build/thirdparty-src"
OUT="$ROOT/build/thirdparty"
mkdir -p "$SRC" "$OUT"
cd "$SRC"

# A download that failed used to fall through to the tar after it, which
# reported a missing archive as a missing config.sub three steps on. ftp.gnu.org
# is the first place GNU publishes and not the only one: on a network where it
# does not answer, kernel.org's mirror of the same tree does.
fetch() {
  [ -f "$2" ] && return 0
  echo "build-thirdparty: fetching $2"
  local url
  for url in "$1" $(echo "$1" | sed -n 's|^https://ftp.gnu.org/gnu/|https://mirrors.kernel.org/gnu/|p'); do
    if curl -sSfL --connect-timeout 30 -o "$2.part" "$url"; then
      mv "$2.part" "$2"
      return 0
    fi
    echo "build-thirdparty: $url did not answer" >&2
  done
  rm -f "$2.part"
  echo "build-thirdparty: could not fetch $2" >&2
  exit 1
}

fetch https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz bzip2-1.0.8.tar.gz
rm -rf bzip2-1.0.8
tar xf bzip2-1.0.8.tar.gz
( cd bzip2-1.0.8 && make -s CC=x86_64-lean_os-gcc AR=x86_64-lean_os-ar \
      RANLIB=x86_64-lean_os-ranlib \
      CFLAGS="-Wall -O2 -D_FILE_OFFSET_BITS=64" bzip2 >bzip2.log 2>&1 ) || {
  echo "build-thirdparty: bzip2 did not build:" >&2
  tail -20 "$SRC/bzip2-1.0.8/bzip2.log" >&2
  exit 1
}
cp bzip2-1.0.8/bzip2 "$OUT/bzip2"
echo "build-thirdparty: bzip2 -> $OUT/bzip2"

fetch https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz hello-2.12.1.tar.gz
rm -rf hello-2.12.1
tar xf hello-2.12.1.tar.gz
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        hello-2.12.1/build-aux/config.sub || exit 1
(
  cd hello-2.12.1
  ./configure --host=x86_64-lean_os --disable-nls >configure.out 2>&1 &&
  make >make.out 2>&1
) || {
  echo "build-thirdparty: GNU hello did not build. The configure log is the" >&2
  echo "                  evidence M94 asked for: $SRC/hello-2.12.1/config.log" >&2
  tail -20 "$SRC/hello-2.12.1/make.out" 2>/dev/null >&2
  exit 1
}
cp hello-2.12.1/hello "$OUT/gnuhello"
echo "build-thirdparty: GNU hello -> $OUT/gnuhello"
echo "build-thirdparty: the configure log is $SRC/hello-2.12.1/config.log"

STAGE="$ROOT/build/thirdparty-sysroot"
SYSROOT="$ROOT/build/sysroot"
mkdir -p "$STAGE"

# Only what changed, by contents: a cp -R rewrites the time of every header
# here, and Chromium's build includes expat's - so a rerun of this script
# under a running Chromium build was a rebuild of whatever included them (M169's
# lesson, met from this side in M227). Symbolic links are re-pointed, which
# costs nothing.
stage_into_sysroot() {
  find "$STAGE" -name '*.la' -delete
  "$ROOT/tools/copy-changed.sh" "$STAGE" "$SYSROOT" > /dev/null || exit 1
  (cd "$STAGE" && find . -type l | sed 's|^\./||') | while IFS= read -r link; do
    mkdir -p "$(dirname "$SYSROOT/$link")"
    ln -sfn "$(readlink "$STAGE/$link")" "$SYSROOT/$link"
  done
}

ZLIB_VER=1.3.1
fetch https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz zlib-$ZLIB_VER.tar.gz
rm -rf "zlib-$ZLIB_VER"
tar xf "zlib-$ZLIB_VER.tar.gz"
(
  cd "zlib-$ZLIB_VER"
  CHOST=x86_64-lean_os CC=x86_64-lean_os-gcc AR=x86_64-lean_os-ar \
    RANLIB=x86_64-lean_os-ranlib ./configure --prefix=/usr \
    > configure.log 2>&1 &&
  make libz.a "libz.so.$ZLIB_VER" example minigzip > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: zlib did not build:" >&2
  tail -20 "$SRC/zlib-$ZLIB_VER/make.log" >&2
  exit 1
}
cp "zlib-$ZLIB_VER/example"  "$OUT/zlibtest"
cp "zlib-$ZLIB_VER/minigzip" "$OUT/minigzip"
cp "zlib-$ZLIB_VER/libz.so.$ZLIB_VER" "$OUT/libz.so.1"
stage_into_sysroot
echo "build-thirdparty: zlib $ZLIB_VER -> $OUT/zlibtest, $OUT/minigzip, and libz in the sysroot"

PNG_VER=1.6.44
fetch https://downloads.sourceforge.net/project/libpng/libpng16/$PNG_VER/libpng-$PNG_VER.tar.gz \
      libpng-$PNG_VER.tar.gz
rm -rf "libpng-$PNG_VER"
tar xf "libpng-$PNG_VER.tar.gz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "libpng-$PNG_VER/config.sub" || exit 1
(
  cd "libpng-$PNG_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr > configure.log 2>&1 &&
  make > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: libpng did not build:" >&2
  tail -20 "$SRC/libpng-$PNG_VER/make.log" >&2
  exit 1
}
cp "libpng-$PNG_VER/pngtest" "$OUT/pngtest"
stage_into_sysroot
echo "build-thirdparty: libpng $PNG_VER -> $OUT/pngtest, and libpng16 in the sysroot"

JPEG_VER=9f
fetch https://www.ijg.org/files/jpegsrc.v$JPEG_VER.tar.gz jpegsrc.v$JPEG_VER.tar.gz
rm -rf jpeg-$JPEG_VER
tar xf "jpegsrc.v$JPEG_VER.tar.gz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "jpeg-$JPEG_VER/config.sub" || exit 1
(
  cd "jpeg-$JPEG_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr > configure.log 2>&1 &&
  make > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: libjpeg did not build:" >&2
  tail -20 "$SRC/jpeg-$JPEG_VER/make.log" >&2
  exit 1
}
cp "jpeg-$JPEG_VER/djpeg"    "$OUT/djpeg"
cp "jpeg-$JPEG_VER/cjpeg"    "$OUT/cjpeg"
cp "jpeg-$JPEG_VER/jpegtran" "$OUT/jpegtran"
stage_into_sysroot
echo "build-thirdparty: libjpeg $JPEG_VER -> $OUT/djpeg, $OUT/cjpeg, $OUT/jpegtran, and libjpeg in the sysroot"

if ! command -v pkgconf >/dev/null 2>&1; then
  echo "build-thirdparty: no pkgconf on this host - freetype's configure needs one" >&2
  echo "                  (brew install pkgconf)" >&2
  exit 1
fi
cat > "$PREFIX/bin/x86_64-lean_os-pkg-config" <<EOF
#!/bin/sh
# x86_64-lean_os-pkg-config - pkgconf, answering for this OS's sysroot.
# Generated by tools/build-thirdparty.sh; see its header for why.
SYSROOT="$SYSROOT"
PKG_CONFIG_LIBDIR="\$SYSROOT/usr/lib/pkgconfig:\$SYSROOT/usr/share/pkgconfig" \\
PKG_CONFIG_SYSROOT_DIR="\$SYSROOT" \\
exec pkgconf --static "\$@"
EOF
chmod +x "$PREFIX/bin/x86_64-lean_os-pkg-config"

FT_VER=2.13.3
fetch https://downloads.sourceforge.net/project/freetype/freetype2/$FT_VER/freetype-$FT_VER.tar.xz \
      freetype-$FT_VER.tar.xz
rm -rf "freetype-$FT_VER"
tar xf "freetype-$FT_VER.tar.xz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "freetype-$FT_VER/builds/unix/config.sub" || exit 1
(
  cd "freetype-$FT_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr \
      --with-zlib=yes --with-png=yes --with-harfbuzz=no \
      --with-bzip2=no --with-brotli=no > configure.log 2>&1 &&
  grep -q 'checking for LIBPNG... yes' configure.log &&
  make > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: freetype did not build:" >&2
  tail -20 "$SRC/freetype-$FT_VER/make.log" >&2
  exit 1
}
stage_into_sysroot
echo "build-thirdparty: freetype $FT_VER -> libfreetype in the sysroot"

HOST="$ROOT/build/thirdparty-host"
mkdir -p "$HOST"
if [ ! -f "$HOST/ft-install/lib/libfreetype.a" ]; then
  echo "build-thirdparty: building freetype $FT_VER for the host, as the oracle"
  (
    cd "$HOST" && rm -rf "freetype-$FT_VER" && tar xf "$SRC/freetype-$FT_VER.tar.xz" &&
    cd "freetype-$FT_VER" &&
    ./configure --prefix="$HOST/ft-install" --with-zlib=no --with-png=no \
        --with-harfbuzz=no --with-bzip2=no --with-brotli=no > configure.log 2>&1 &&
    make > make.log 2>&1 && make install > install.log 2>&1
  ) || { echo "build-thirdparty: the host freetype did not build" >&2; exit 1; }
fi

DEJAVU_VER=2.37
fetch https://downloads.sourceforge.net/project/dejavu/dejavu/$DEJAVU_VER/dejavu-fonts-ttf-$DEJAVU_VER.tar.bz2 \
      dejavu-fonts-ttf-$DEJAVU_VER.tar.bz2
rm -rf "dejavu-fonts-ttf-$DEJAVU_VER"
tar xf "dejavu-fonts-ttf-$DEJAVU_VER.tar.bz2"
FONT="$SRC/dejavu-fonts-ttf-$DEJAVU_VER/ttf/DejaVuSans.ttf"

cc -O2 -Wall -Wextra -o "$OUT/ftrender-host" "$ROOT/tests/freetype/ftrender.c" \
   -I"$HOST/ft-install/include/freetype2" "$HOST/ft-install/lib/libfreetype.a" || exit 1
x86_64-lean_os-gcc -O2 -Wall -Wextra -o "$OUT/ftrender" "$ROOT/tests/freetype/ftrender.c" \
   $(x86_64-lean_os-pkg-config --cflags --libs freetype2) || {
  echo "build-thirdparty: tests/freetype/ftrender.c did not build against the sysroot's freetype" >&2
  exit 1
}
"$OUT/ftrender-host" "$FONT" > "$OUT/ftrender.expected" || {
  echo "build-thirdparty: the host freetype could not render the font" >&2
  exit 1
}
echo "build-thirdparty: freetype's oracle -> $(wc -l < "$OUT/ftrender.expected" | tr -d ' ') lines of reference output, $(tail -1 "$OUT/ftrender.expected")"

EXPAT_VER=2.6.4
fetch https://github.com/libexpat/libexpat/releases/download/R_$(echo $EXPAT_VER | tr . _)/expat-$EXPAT_VER.tar.xz \
      expat-$EXPAT_VER.tar.xz
rm -rf "expat-$EXPAT_VER"
tar xf "expat-$EXPAT_VER.tar.xz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "expat-$EXPAT_VER/conftools/config.sub" || exit 1
(
  cd "expat-$EXPAT_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr \
      --without-docbook --without-examples > configure.log 2>&1 &&
  make > make.log 2>&1 &&
  make -C tests runtests > tests.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: expat did not build:" >&2
  tail -20 "$SRC/expat-$EXPAT_VER/make.log" >&2
  exit 1
}
cp "expat-$EXPAT_VER/tests/runtests" "$OUT/expattest"
cp "expat-$EXPAT_VER/xmlwf/xmlwf"    "$OUT/xmlwf"
stage_into_sysroot
echo "build-thirdparty: expat $EXPAT_VER -> $OUT/expattest, $OUT/xmlwf, and libexpat in the sysroot"

SQLITE_VER=3470200
fetch https://www.sqlite.org/2024/sqlite-autoconf-$SQLITE_VER.tar.gz \
      sqlite-autoconf-$SQLITE_VER.tar.gz
rm -rf "sqlite-autoconf-$SQLITE_VER"
tar xf "sqlite-autoconf-$SQLITE_VER.tar.gz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "sqlite-autoconf-$SQLITE_VER/config.sub" || exit 1
(
  cd "sqlite-autoconf-$SQLITE_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr --disable-shared \
      --disable-readline --disable-editline \
      --disable-dynamic-extensions > configure.log 2>&1 &&
  make > make.log 2>&1 &&
  make install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: sqlite did not build:" >&2
  tail -20 "$SRC/sqlite-autoconf-$SQLITE_VER/make.log" >&2
  exit 1
}
cp "sqlite-autoconf-$SQLITE_VER/sqlite3" "$OUT/sqlite3"
stage_into_sysroot
echo "build-thirdparty: sqlite $SQLITE_VER -> $OUT/sqlite3, and libsqlite3 in the sysroot"

if [ ! -x "$HOST/sqlite-install/bin/sqlite3" ]; then
  echo "build-thirdparty: building sqlite $SQLITE_VER for the host, as the oracle"
  (
    cd "$HOST" && rm -rf "sqlite-autoconf-$SQLITE_VER" &&
    tar xf "$SRC/sqlite-autoconf-$SQLITE_VER.tar.gz" &&
    cd "sqlite-autoconf-$SQLITE_VER" &&
    ./configure --prefix="$HOST/sqlite-install" --disable-shared \
        --disable-readline --disable-editline > configure.log 2>&1 &&
    make > make.log 2>&1 && make install > install.log 2>&1
  ) || { echo "build-thirdparty: the host sqlite did not build" >&2; exit 1; }
fi
rm -f "$OUT/sqlite-host.db"
"$HOST/sqlite-install/bin/sqlite3" -batch -bail "$OUT/sqlite-host.db" \
    < "$ROOT/tests/sqlite/cases.sql" > "$OUT/sqlite.expected" 2>&1 || {
  echo "build-thirdparty: the host sqlite did not accept tests/sqlite/cases.sql:" >&2
  tail -5 "$OUT/sqlite.expected" >&2
  exit 1
}
rm -f "$OUT/sqlite-host.db"
echo "build-thirdparty: sqlite's oracle -> $(wc -l < "$OUT/sqlite.expected" | tr -d ' ') lines of transcript"

HB_VER=8.5.0
fetch https://github.com/harfbuzz/harfbuzz/releases/download/$HB_VER/harfbuzz-$HB_VER.tar.xz \
      harfbuzz-$HB_VER.tar.xz
rm -rf "harfbuzz-$HB_VER"
tar xf "harfbuzz-$HB_VER.tar.xz"
python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
        "harfbuzz-$HB_VER/config.sub" || exit 1
HB_OPTS="--enable-static --disable-shared --with-freetype=yes --with-glib=no \
         --with-gobject=no --with-cairo=no --with-icu=no --with-graphite2=no \
         --with-chafa=no"
(
  cd "harfbuzz-$HB_VER"
  ./configure --host=x86_64-lean_os --prefix=/usr $HB_OPTS > configure.log 2>&1 &&
  grep -q 'FreeType:.*true' configure.log &&
  make -C src > make.log 2>&1 &&
  make -C src install DESTDIR="$STAGE" > install.log 2>&1
) || {
  echo "build-thirdparty: harfbuzz did not build:" >&2
  grep -h 'error' "$SRC/harfbuzz-$HB_VER/make.log" 2>/dev/null | sort | uniq -c | sort -rn | head -10 >&2
  exit 1
}
stage_into_sysroot
echo "build-thirdparty: harfbuzz $HB_VER -> libharfbuzz in the sysroot"

if [ ! -f "$HOST/hb-install/lib/libharfbuzz.a" ]; then
  echo "build-thirdparty: building harfbuzz $HB_VER for the host, as the oracle"
  (
    cd "$HOST" && rm -rf "harfbuzz-$HB_VER" && tar xf "$SRC/harfbuzz-$HB_VER.tar.xz" &&
    cd "harfbuzz-$HB_VER" &&
    PKG_CONFIG_PATH="$HOST/ft-install/lib/pkgconfig" \
      ./configure --prefix="$HOST/hb-install" $HB_OPTS --with-coretext=no > configure.log 2>&1 &&
    make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" -C src > make.log 2>&1 &&
    make -C src install > install.log 2>&1
  ) || { echo "build-thirdparty: the host harfbuzz did not build" >&2; exit 1; }
fi
c++ -O2 -Wall -Wextra -o "$OUT/hbshape-host" "$ROOT/tests/harfbuzz/hbshape.c" \
   -I"$HOST/hb-install/include/harfbuzz" -I"$HOST/ft-install/include/freetype2" \
   "$HOST/hb-install/lib/libharfbuzz.a" "$HOST/ft-install/lib/libfreetype.a" || exit 1
x86_64-lean_os-g++ -O2 -Wall -Wextra -o "$OUT/hbshape" "$ROOT/tests/harfbuzz/hbshape.c" \
   $(x86_64-lean_os-pkg-config --cflags --libs harfbuzz freetype2) || {
  echo "build-thirdparty: tests/harfbuzz/hbshape.c did not build against the sysroot's harfbuzz" >&2
  exit 1
}
"$OUT/hbshape-host" "$FONT" > "$OUT/hbshape.expected" || {
  echo "build-thirdparty: the host harfbuzz could not shape the fixture's text" >&2
  exit 1
}
echo "build-thirdparty: harfbuzz's oracle -> $(wc -l < "$OUT/hbshape.expected" | tr -d ' ') lines of reference output, $(tail -1 "$OUT/hbshape.expected")"

MBEDTLS_VER=3.6.2
fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER.tar.bz2 \
      mbedtls-$MBEDTLS_VER.tar.bz2
rm -rf "mbedtls-$MBEDTLS_VER"
tar xf "mbedtls-$MBEDTLS_VER.tar.bz2"
MBEDTLS_SUITES="test_suite_chachapoly test_suite_shax test_suite_ecdsa"
(
  cd "mbedtls-$MBEDTLS_VER"
  make -C library CC=x86_64-lean_os-gcc AR=x86_64-lean_os-ar static > lib.log 2>&1 &&
  make -C programs CC=x86_64-lean_os-gcc AR=x86_64-lean_os-ar \
       ssl/ssl_client2 ssl/ssl_server2 > programs.log 2>&1 &&
  make -C tests generated_files > gen.log 2>&1 &&
  make -C tests CC=x86_64-lean_os-gcc AR=x86_64-lean_os-ar $MBEDTLS_SUITES > suites.log 2>&1 &&
  mkdir -p "$STAGE/usr/include" "$STAGE/usr/lib" &&
  cp -rp include/mbedtls include/psa "$STAGE/usr/include/" &&
  cp -p library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a "$STAGE/usr/lib/"
) || {
  echo "build-thirdparty: mbedtls did not build:" >&2
  for l in lib programs suites; do
    grep -h 'error' "$SRC/mbedtls-$MBEDTLS_VER/$l.log" 2>/dev/null | sort | uniq -c | sort -rn | head -5 >&2
  done
  exit 1
}
cp "mbedtls-$MBEDTLS_VER/programs/ssl/ssl_client2" "$OUT/ssl_client2"
cp "mbedtls-$MBEDTLS_VER/programs/ssl/ssl_server2" "$OUT/ssl_server2"
MBEDTLS_DATA="$OUT/mbedtls-suites"
rm -rf "$MBEDTLS_DATA"
mkdir -p "$MBEDTLS_DATA"
for suite in $MBEDTLS_SUITES; do
  cp "mbedtls-$MBEDTLS_VER/tests/$suite" "$MBEDTLS_DATA/$suite"
  cp "mbedtls-$MBEDTLS_VER/tests/$suite.datax" "$MBEDTLS_DATA/$suite.datax"
done
stage_into_sysroot
echo "build-thirdparty: mbedtls $MBEDTLS_VER -> $OUT/ssl_server2, $OUT/ssl_client2, three of its own suites, and libmbedtls in the sysroot"

cat > "$OUT/printca.c" <<EOF
#include <stdio.h>
#include "test/certs.h"
int main(void) { fputs(mbedtls_test_cas_pem, stdout); return 0; }
EOF
cc -o "$OUT/printca" "$OUT/printca.c" -I"mbedtls-$MBEDTLS_VER/include" \
   -I"mbedtls-$MBEDTLS_VER/tests/include" -I"mbedtls-$MBEDTLS_VER/library" \
   "mbedtls-$MBEDTLS_VER/tests/src/certs.c" || exit 1
"$OUT/printca" > "$OUT/mbedtls-test-ca.pem" || exit 1

x86_64-lean_os-gcc -O2 -Wall -Wextra -o "$OUT/httpsget" "$ROOT/tests/tls/httpsget.c" \
   -lmbedtls -lmbedx509 -lmbedcrypto || {
  echo "build-thirdparty: tests/tls/httpsget.c did not build against the sysroot's mbedtls" >&2
  exit 1
}
echo "build-thirdparty: httpsget -> $OUT/httpsget, and the test CA as $OUT/mbedtls-test-ca.pem"

DATA="$OUT/m100-data"
rm -rf "$DATA"
mkdir -p "$DATA"
for f in testorig.jpg testprog.jpg testimg.ppm testimg.jpg testimgp.jpg \
         testimg.gif testimg.bmp; do
  cp "jpeg-$JPEG_VER/$f" "$DATA/$f"
done
cp "libpng-$PNG_VER/pngtest.png" "$DATA/pngtest.png"
cp "$FONT" "$DATA/DejaVuSans.ttf"
cp "$OUT/ftrender.expected" "$DATA/ftrender.expected"
cp "$ROOT/tests/sqlite/cases.sql" "$DATA/cases.sql"
cp "$OUT/sqlite.expected" "$DATA/sqlite.expected"
cp "$OUT/hbshape.expected" "$DATA/hbshape.expected"
cp "$OUT/mbedtls-test-ca.pem" "$DATA/mbedtls-test-ca.pem"
echo "build-thirdparty: the reference output their own suites compare against -> $DATA"
