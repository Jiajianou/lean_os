#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

PREFIX="${LEANOS_TOOLCHAIN_PREFIX:-$ROOT/build/toolchain}"
if [ ! -x "$PREFIX/bin/x86_64-lean_os-gcc" ]; then
  echo "build-netsurf: no x86_64-lean_os-gcc - run tools/build-toolchain.sh" >&2
  exit 1
fi

BISON_DIR=""
for d in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin; do
  [ -x "$d/bison" ] && BISON_DIR="$d" && break
done
export PATH="${BISON_DIR:+$BISON_DIR:}$PREFIX/bin:$PATH"
BISON_VER=$(bison --version 2>/dev/null | head -1 | grep -oE '[0-9]+' | head -1)
if [ -z "$BISON_VER" ] || [ "$BISON_VER" -lt 3 ]; then
  echo "build-netsurf: need bison 3.x - libnslog's grammar uses" >&2
  echo "               '%destructor { } <tag>', which bison 2.3 (what" >&2
  echo "               macOS ships) cannot parse. 'brew install bison'." >&2
  exit 1
fi

HOST_PNG_CFLAGS=""
HOST_PNG_LDFLAGS="-lpng"
for d in /opt/homebrew/opt/libpng /usr/local/opt/libpng; do
  if [ -f "$d/include/png.h" ]; then
    HOST_PNG_CFLAGS="-I$d/include"
    HOST_PNG_LDFLAGS="-L$d/lib -lpng"
    break
  fi
done

make -s -C "$ROOT" sysroot >/dev/null || exit 1

SRC="$ROOT/build/thirdparty-src"
STAGE="$ROOT/build/thirdparty-sysroot"
SYSROOT="$ROOT/build/sysroot"
OUT="$ROOT/build/netsurf"
mkdir -p "$SRC" "$STAGE" "$OUT"
cd "$SRC"

fetch() {
  [ -f "$2" ] && return 0
  echo "build-netsurf: fetching $2"
  curl -sSL -o "$2.part" "$1" && mv "$2.part" "$2"
}

NS_VER=3.11
CURL_VER=8.11.1

fetch https://curl.se/download/curl-$CURL_VER.tar.xz curl-$CURL_VER.tar.xz
if [ ! -f "$STAGE/usr/lib/libcurl.a" ]; then
  rm -rf "curl-$CURL_VER"
  tar xf "curl-$CURL_VER.tar.xz"
  python3 "$ROOT/tools/toolchain-port/apply.py" --config-sub \
          "curl-$CURL_VER/config.sub" || exit 1
  (
    cd "curl-$CURL_VER" || exit 1
    CC="x86_64-lean_os-gcc -isystem $SYSROOT/usr/local/include" \
    ./configure --host=x86_64-lean_os --prefix=/usr \
        --disable-shared --enable-static \
        --with-mbedtls --with-zlib --without-libpsl \
        --disable-ldap --disable-ldaps --disable-threaded-resolver \
        --disable-ipv6 --disable-manual \
        --without-brotli --without-zstd >configure.log 2>&1 || {
      echo "build-netsurf: curl's configure failed:" >&2
      tail -30 configure.log >&2
      exit 1
    }
    make -j4 >build.log 2>&1 || {
      echo "build-netsurf: curl did not build:" >&2
      grep -E "error:" build.log | head -20 >&2
      exit 1
    }
    make install DESTDIR="$STAGE" >install.log 2>&1 || exit 1
  ) || exit 1
  rm -f "$STAGE"/usr/lib/*.la
  make -s -C "$ROOT" sysroot >/dev/null || exit 1
  echo "build-netsurf: libcurl $CURL_VER (mbedTLS) -> $STAGE/usr/lib/libcurl.a"
fi

fetch https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-$NS_VER.tar.gz \
      netsurf-all-$NS_VER.tar.gz
NSDIR="$SRC/netsurf-all-$NS_VER"
if [ ! -d "$NSDIR" ]; then
  tar xf "netsurf-all-$NS_VER.tar.gz"
fi
python3 "$ROOT/tools/netsurf-port/apply.py" "$NSDIR" || exit 1

cd "$NSDIR"

export CFLAGS="-DWITHOUT_ICONV_FILTER -D_GNU_SOURCE -fno-omit-frame-pointer"

NSLIBS="libwapcaplet libnslog libparserutils libcss libhubbub libdom \
        libnsbmp libnsgif librosprite libnsutils libutf8proc libnspsl \
        libsvgtiny libnsfb"

for L in $NSLIBS; do
  make -C "$L" install HOST=x86_64-lean_os NSSHARED="$NSDIR/buildsystem" \
       PREFIX=/usr DESTDIR="$STAGE" Q=@ WARNFLAGS='-Wall -W -Wno-error' \
       PKG_CONFIG=x86_64-lean_os-pkg-config >"$OUT/$L.log" 2>&1 || {
    echo "build-netsurf: $L did not build:" >&2
    tail -20 "$OUT/$L.log" >&2
    exit 1
  }
done
echo "build-netsurf: 14 NetSurf libraries -> $STAGE/usr/lib"

make -C nsgenbind install NSSHARED="$NSDIR/buildsystem" \
     PREFIX="$NSDIR/inst-host" DESTDIR= Q=@ >"$OUT/nsgenbind.log" 2>&1 || {
  echo "build-netsurf: nsgenbind (host tool) did not build:" >&2
  tail -20 "$OUT/nsgenbind.log" >&2
  exit 1
}
export PATH="$NSDIR/inst-host/bin:$PATH"

make -s -C "$ROOT" sysroot >/dev/null || exit 1
x86_64-lean_os-gcc -O2 -std=c99 -Wall -Wextra -Werror -c \
  -I "$NSDIR/libnsfb/include" -I "$NSDIR/libnsfb/src" \
  -I "$ROOT/system_api/include" -I "$ROOT/user_space/library" \
  -o "$OUT/nsfb_leanos.o" "$ROOT/user_space/binaries/nsfb_leanos.c" || {
  echo "build-netsurf: the lean_os surface did not compile" >&2
  exit 1
}
x86_64-lean_os-ar r "$STAGE/usr/lib/libnsfb.a" "$OUT/nsfb_leanos.o" || exit 1
make -s -C "$ROOT" sysroot >/dev/null || exit 1
echo "build-netsurf: the lean_os surface -> libnsfb.a"

make -C netsurf TARGET=framebuffer HOST=x86_64-lean_os PREFIX=/usr \
     NSSHARED="$NSDIR/buildsystem" Q=@ WARNFLAGS='-Wall -W -Wno-error' \
     GCCSDK_INSTALL_CROSSBIN="$PREFIX/bin" \
     PKG_CONFIG=x86_64-lean_os-pkg-config \
     PKGCONFIG=x86_64-lean_os-pkg-config \
     BUILD_LIBPNG_CFLAGS="$HOST_PNG_CFLAGS" \
     BUILD_LIBPNG_LDFLAGS="$HOST_PNG_LDFLAGS" \
     NETSURF_USE_CURL=YES NETSURF_USE_OPENSSL=NO \
     NETSURF_USE_WEBP=NO NETSURF_USE_JPEGXL=NO NETSURF_USE_HARU_PDF=NO \
     NETSURF_USE_LIBICONV_PLUG=YES NETSURF_FB_FONTLIB=freetype \
     >"$OUT/netsurf.log" 2>&1 || {
  echo "build-netsurf: NetSurf did not build:" >&2
  grep -E "error:|undefined reference" "$OUT/netsurf.log" | head -20 >&2
  exit 1
}

python3 - "$PREFIX/bin/x86_64-lean_os-nm" \
         "$PREFIX/bin/x86_64-lean_os-readelf" netsurf/nsfb <<'PYCHECK' || exit 1
import re, subprocess, sys
nm, readelf, binary = sys.argv[1], sys.argv[2], sys.argv[3]

syms = {}
for line in subprocess.run([nm, binary], capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[2].endswith("_register_surface"):
        syms[int(parts[0], 16)] = parts[2]

words = []
for line in subprocess.run([readelf, "-x", ".init_array", binary],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r"\s+0x[0-9a-f]+((?:\s[0-9a-f]{8})+)", line)
    if m:
        for i, w in enumerate(m.group(1).split()):
            words.append(w)
ptrs = [int.from_bytes(bytes.fromhex(words[i] + words[i + 1]), "little")
        for i in range(0, len(words) - 1, 2)]
registered = [syms[p] for p in ptrs if p in syms]
if not registered:
    print("build-netsurf: no surface constructors found in .init_array - the",
          file=sys.stderr)
    print("               check itself has stopped working, which is worse",
          file=sys.stderr)
    print("               than the thing it checks. Look at it.", file=sys.stderr)
    sys.exit(1)
if registered[-1] != "leanos_register_surface":
    print("build-netsurf: the last surface to register is %s, not this" %
          registered[-1], file=sys.stderr)
    print("               project's. A bare `netsurf` would come up on it and", file=sys.stderr)
    print("               draw into a buffer nothing displays. Order in", file=sys.stderr)
    print("               .init_array: %s" % ", ".join(registered), file=sys.stderr)
    sys.exit(1)
print("build-netsurf: surface constructors in order: %s - a bare `netsurf`"
      % ", ".join(s.replace("_register_surface", "") for s in registered))
print("               picks leanos")
PYCHECK

cp netsurf/nsfb "$OUT/netsurf"

RES="$OUT/res"
rm -rf "$RES"
mkdir -p "$RES"
for f in adblock.css credits.html default.css internal.css licence.html \
         netsurf.png quirks.css welcome.html favicon.png; do
  cp "netsurf/frontends/framebuffer/res/$f" "$RES/" || exit 1
done
cp netsurf/frontends/framebuffer/res/en/Messages "$RES/Messages" || exit 1

cat > "$RES/Choices" <<'CHOICES'
# Choices - NetSurf's options on lean_os. Written by
# tools/build-netsurf.sh; see its header for why this file exists at all
# rather than being command-line flags.
enable_javascript:1
# The local welcome page, so a browser started with no argument shows
# something on a machine whose network may not be up yet.
homepage_url:file:///usr/share/netsurf/welcome.html
# 12.8pt. NetSurf stores font size in tenths of a point.
font_size:128
# The window. NetSurf's own window_width/window_height options override
# whatever the surface reports as its default (see leanos_defaults in
# user_space/binaries/nsfb_leanos.c, whose 900x640 is therefore only what a
# build with no Choices file would get). Sized to leave the taskbar and
# the icon column visible on this machine's 1024x768 default.
window_width:800
window_height:600
# Anti-aliased text. The compositor's window is 32bpp and freetype's
# grey rendering is what M100's [m100c] self-test already grades.
fb_font_monochrome:0
# ---- https, and where the certificate authorities come from ----------
# CURLOPT_CAINFO, pointed at a PACKAGE rather than at a file in the
# image, and that is the decision rather than an accident.
#
# This OS still ships no certificate authorities: a fresh image has
# nothing at this path and https fails cert verification, which is
# M65's rule (docs/browser.md). What M114 changed is that there is now
# a supported way to add them that is not "rebuild the OS" -
#
#     os install ca-certificates
#
# - which was the exact condition M100 recorded for reopening this, and
# M111's package manager is what met it. A bundle has to be updatable on
# its own, because an authority is removed from one when it has done
# something wrong.
#
# The version is in the path because /pkg/<name>/<version>/ is where an
# installed package lives (docs/packages.md). It is pinned here and in
# tools/build-packages.sh, and those two are the only places it appears.
ca_bundle:/pkg/ca-certificates/1.0/share/ca-certificates/ca-bundle.pem
CHOICES

DEJAVU=$(echo "$SRC"/dejavu-fonts-ttf-*/ttf/DejaVuSans.ttf | head -1)
if [ ! -f "$DEJAVU" ]; then
  echo "build-netsurf: no DejaVu fonts - run tools/build-thirdparty.sh" >&2
  echo "               first; it downloads them for [m100c]." >&2
  exit 1
fi
mkdir -p "$RES/fonts"
cp "$(dirname "$DEJAVU")"/DejaVuSans.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSans-Bold.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSans-Oblique.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSans-BoldOblique.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSerif.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSerif-Bold.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSansMono.ttf \
   "$(dirname "$DEJAVU")"/DejaVuSansMono-Bold.ttf "$RES/fonts/" || exit 1

SZ=$(( $(wc -c < "$OUT/netsurf") / 1024 ))
echo "build-netsurf: /bin/netsurf -> $OUT/netsurf (${SZ}K)"
echo "build-netsurf: resources    -> $RES"
echo "build-netsurf: now run tools/install-netsurf.sh to put it on the disk"
