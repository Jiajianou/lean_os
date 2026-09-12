#!/bin/sh
# tools/build-netsurf-host.sh - M117: the same NetSurf, built for THIS Mac.
#
# M116 left the browser's first crash with a condition: "the same NetSurf
# 3.11, built for the host, against the page, decides whose failure it
# is - upstream's, or this port's". Meeting it took a dozen workarounds
# and, the first three times, produced a build that was NOT the same
# NetSurf: the freetype and JPEG switches were being overwritten by the
# script that set them, and the twin rendered apple.com for reasons that
# had nothing to do with the guest. This script is that condition made
# repeatable - one command, and the binary it produces differs from
# /bin/netsurf in exactly the ways listed here and no others:
#
#   - clang for arm64 rather than x86_64-lean_os-gcc; macOS's libc, curl
#     and iconv rather than this project's. That is the point: the
#     framebuffer front end, libnsfb's display-less `ram` surface, the
#     freetype font layer with the GUEST's own DejaVu files, the JPEG
#     decoder from the same jpeg-9f tarball, Duktape, and the guest's
#     Choices are all the same.
#   - NETSURF_USE_OPENSSL=NO: Apple's libcurl carries LibreSSL and
#     NetSurf's fetcher would poke its SSL context through Homebrew's
#     OpenSSL 3 - a SIGBUS inside libssl before the first byte. Without
#     the flag NetSurf leaves TLS to curl, which is what the guest does
#     with mbedtls anyway.
#
# Everything it edits is a scratch copy under build/netsurf-host/. Two
# NetSurf Makefiles get four edits for Apple's linker (no --whole-archive,
# no --trace) and one for the monkey front end's -Werror under clang; the
# source is not touched. build/thirdparty-src must already hold the
# tarballs (tools/build-netsurf.sh fetches them) and `brew install bison`
# is needed, as it is for the port itself.
#
#   tools/build-netsurf-host.sh              build (idempotent)
#   tools/build-netsurf-host.sh run <url>    load <url> at 800x600 for 45 s
#                                            and report rendered / crashed
#
# `run` is the instrument: it exits 1 on an assertion or a signal, 0 if
# the page rendered, and leaves the full -v log in build/netsurf-host/run.log.
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

# ---- run --------------------------------------------------------------
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

# ---- build -------------------------------------------------------------
mkdir -p "$HOST"
if [ ! -d "$NSDIR" ]; then
  [ -f "$SRC/netsurf-all-$NS_VER.tar.gz" ] || { echo "build-netsurf-host: no $SRC/netsurf-all-$NS_VER.tar.gz - run tools/build-netsurf.sh first" >&2; exit 1; }
  tar xf "$SRC/netsurf-all-$NS_VER.tar.gz" -C "$HOST"
  # Apple's ld: no --whole-archive (-all_load instead), no --trace.
  sed -i '' -e 's/^LDFLAGS += -Wl,--whole-archive$/LDFLAGS += -Wl,-all_load/' \
            -e '/^LDFLAGS += -Wl,--no-whole-archive$/d' "$NSDIR/netsurf/frontends/framebuffer/Makefile"
  sed -i '' -e 's/-Wl,--trace//' "$NSDIR/netsurf/Makefile"
  sed -i '' -e 's/^CWARNFLAGS += -Werror/CWARNFLAGS +=/' "$NSDIR/netsurf/frontends/monkey/Makefile"
fi

# The guest's freetype and libjpeg, from the same tarballs, static, private.
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

# The switches, in the one file NetSurf reads for them. Written by THIS
# script and only this script - the build once had two writers and the
# second one won silently, which is how the first twins were not twins.
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

# The guest's Choices, with the certificate bundle pointed at the host's.
mkdir -p "$HOST/res"
[ -f "$ROOT/build/netsurf/res/Choices" ] || { echo "build-netsurf-host: no build/netsurf/res/Choices - the port has not been built" >&2; exit 1; }
grep -v '^ca_bundle' "$ROOT/build/netsurf/res/Choices" > "$HOST/res/Choices"
echo "ca_bundle:/etc/ssl/cert.pem" >> "$HOST/res/Choices"

for sym in FT_Init_FreeType jpeg_read_header duk_create_heap; do
  nm "$NSFB" | grep -q "$sym" || { echo "build-netsurf-host: $sym is not in the binary - this is not the guest's NetSurf" >&2; exit 1; }
done
echo "build-netsurf-host: $NSFB ($(( $(wc -c < "$NSFB") / 1024 ))K) - freetype, jpeg and duktape confirmed"
echo "                    $0 run <url>"
