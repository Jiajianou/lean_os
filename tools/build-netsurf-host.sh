#!/bin/sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/build/thirdparty-src"
HOST="$ROOT/build/netsurf-host"
NS_VER=3.11
NSDIR="$HOST/netsurf-all-$NS_VER"
NSFB="$NSDIR/netsurf/nsfb"

for d in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin; do
  [ -x "$d/bison" ] && export PATH="$d:$PATH" && break
done
PNG_DIR=""
for d in /opt/homebrew/opt/libpng /usr/local/opt/libpng; do
  [ -f "$d/include/png.h" ] && PNG_DIR="$d" && break
done

if [ "$1" = "run" ]; then
  URL="$2"
  [ -n "$URL" ] || { echo "usage: $0 run <url>" >&2; exit 2; }
  [ -x "$NSFB" ] || { echo "build-netsurf-host: not built - run $0 first" >&2; exit 2; }
  cd "$NSDIR/netsurf"
  NETSURFRES="$HOST/res" python3 - "$NSFB" "$URL" "$HOST/run.log" <<'PY'
import subprocess, sys, os
nsfb, url, logpath = sys.argv[1:4]
try:
    p = subprocess.run([nsfb, "-v", "-f", "ram", "-w", "800", "-h", "600", url], capture_output=True, timeout=45)
    out = (p.stdout + p.stderr).decode(errors="replace"); ended = "exited %d" % p.returncode
except subprocess.TimeoutExpired as e:
    out = ((e.stdout or b"") + (e.stderr or b"")).decode(errors="replace"); ended = "still running after 45 s"
open(logpath, "w").write(out)
lines = out.splitlines()
crashed = [l for l in lines if "assert" in l.lower() or "Caught signal" in l]
redraws = sum(1 for l in lines if "content_scaled_redraw" in l)
if crashed:
    print("build-netsurf-host: CRASHED - %s" % crashed[0].strip()); sys.exit(1)
print("build-netsurf-host: rendered (%d redraws, %s, %d log lines) - %s" % (redraws, ended, len(lines), logpath))
PY
  exit $?
fi

mkdir -p "$HOST"
if [ ! -d "$NSDIR" ]; then
  [ -f "$SRC/netsurf-all-$NS_VER.tar.gz" ] || { echo "build-netsurf-host: no $SRC/netsurf-all-$NS_VER.tar.gz - run tools/build-netsurf.sh first" >&2; exit 1; }
  tar xf "$SRC/netsurf-all-$NS_VER.tar.gz" -C "$HOST"
  sed -i '' -e 's/^LDFLAGS += -Wl,--whole-archive$/LDFLAGS += -Wl,-all_load/' \
            -e '/^LDFLAGS += -Wl,--no-whole-archive$/d' "$NSDIR/netsurf/frontends/framebuffer/Makefile"
  sed -i '' -e 's/-Wl,--trace//' "$NSDIR/netsurf/Makefile"
  sed -i '' -e 's/^CWARNFLAGS += -Werror/CWARNFLAGS +=/' "$NSDIR/netsurf/frontends/monkey/Makefile"
fi

if [ ! -f "$HOST/ft/lib/pkgconfig/freetype2.pc" ]; then
  rm -rf "$HOST/ft-src"; mkdir -p "$HOST/ft-src"
  tar xf "$SRC"/freetype-*.tar.xz -C "$HOST/ft-src"
  (cd "$HOST"/ft-src/freetype-* && ./configure --prefix="$HOST/ft" --without-harfbuzz --without-brotli \
      --without-bzip2 --without-png --without-zlib --disable-shared >/dev/null && make -j8 >/dev/null && make install >/dev/null)
fi
if [ ! -f "$HOST/jpeg/lib/libjpeg.a" ]; then
  rm -rf "$HOST/jpeg-src"; mkdir -p "$HOST/jpeg-src"
  tar xf "$SRC"/jpegsrc.v*.tar.gz -C "$HOST/jpeg-src"
  (cd "$HOST"/jpeg-src/jpeg-* && ./configure --prefix="$HOST/jpeg" --disable-shared >/dev/null && make -j8 >/dev/null && make install >/dev/null)
fi

cat > "$NSDIR/netsurf/Makefile.config" <<CONF
COMMON_WARNFLAGS += -Wno-unknown-warning-option -Wno-missing-prototypes -include strings.h -D_DARWIN_C_SOURCE -I$HOST/jpeg/include
NETSURF_USE_JPEG := YES
NETSURF_USE_OPENSSL := NO
NETSURF_FB_FONTLIB := freetype
NETSURF_FB_FONTPATH := $ROOT/build/netsurf/res/fonts
LDFLAGS += -liconv -L$HOST/jpeg/lib
CONF

export PKG_CONFIG_PATH="$HOST/ft/lib/pkgconfig:$HOST/jpeg/lib/pkgconfig"
cd "$NSDIR"
make TARGET=framebuffer ${PNG_DIR:+BUILD_LIBPNG_CFLAGS=-I$PNG_DIR/include} \
     ${PNG_DIR:+"BUILD_LIBPNG_LDFLAGS=-L$PNG_DIR/lib -lpng"} > "$HOST/build.log" 2>&1 || {
  echo "build-netsurf-host: the build failed:" >&2; tail -20 "$HOST/build.log" >&2; exit 1
}

mkdir -p "$HOST/res"
[ -f "$ROOT/build/netsurf/res/Choices" ] || { echo "build-netsurf-host: no build/netsurf/res/Choices - the port has not been built" >&2; exit 1; }
grep -v '^ca_bundle' "$ROOT/build/netsurf/res/Choices" > "$HOST/res/Choices"
echo "ca_bundle:/etc/ssl/cert.pem" >> "$HOST/res/Choices"

for sym in FT_Init_FreeType jpeg_read_header duk_create_heap; do
  nm "$NSFB" | grep -q "$sym" || { echo "build-netsurf-host: $sym is not in the binary - this is not the guest's NetSurf" >&2; exit 1; }
done
echo "build-netsurf-host: $NSFB ($(( $(wc -c < "$NSFB") / 1024 ))K) - freetype, jpeg and duktape confirmed"
echo "                    $0 run <url>"
