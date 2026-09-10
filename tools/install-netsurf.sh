#!/usr/bin/env bash
# tools/install-netsurf.sh - M100: put the browser on the disk.
#
# The install half of tools/build-netsurf.sh, in the same shape as
# tools/install-python.sh and tools/install-native-toolchain.sh: it
# skips with a message when the build has not happened (an image without
# a browser is a valid image), it strips on the way in, and it is a
# separate step from `all` because writing into a fresh image claims
# inodes kernel.c's M22 self-test has opinions about.
#
# /bin/netsurf, not /pkg. That is the difference between a program this
# OS ships and one installed later, and it is the difference the
# capability model turns on: M111 made every binary under /pkg get
# CAP_PKG_UNLISTED - zero - whatever it is called, so a browser there
# could not open a socket. This one is in the shipped table
# (system_api/include/caps.h) with CAP_NETWORK and nothing else.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

OUT=build/netsurf
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT=build/leanfs-put
CROSS_STRIP=build/toolchain/bin/x86_64-lean_os-strip

if [ ! -f "$OUT/netsurf" ]; then
  echo "install-netsurf: no $OUT/netsurf - run tools/build-netsurf.sh first."
  echo "                 (An image without a browser is a valid image.)"
  exit 0
fi
if [ ! -f "$IMAGE" ]; then
  echo "install-netsurf: no $IMAGE - run make first" >&2
  exit 1
fi
[ -x "$PUT" ] || make -s leanfs-put || exit 1

# Stripped. 20 MB of a 2 GiB disk is not the reason - the reason is that
# every byte of it is demand-paged off this disk on the way to the first
# frame, and M92's cache is what pays for the symbols nobody reads.
BIN="$OUT/netsurf.stripped"
cp "$OUT/netsurf" "$BIN"
if [ -x "$CROSS_STRIP" ]; then
  "$CROSS_STRIP" "$BIN" 2>/dev/null || true
fi

"$PUT" "$IMAGE" "$BIN" /bin/netsurf >/dev/null || exit 1
before=$(wc -c < "$OUT/netsurf")
after=$(wc -c < "$BIN")
echo "install-netsurf: /bin/netsurf ($((after / 1024))K, was $((before / 1024))K unstripped)"

# The resources, under /usr/share/netsurf - which is where the compiled-
# in NETSURF_FB_RESPATH looks, because the build ran with PREFIX=/usr.
# Nothing here is a path typed by hand on the command line.
"$PUT" -r "$IMAGE" "$OUT/res" /usr/share/netsurf >/dev/null || exit 1
n=$(find "$OUT/res" -type f | wc -l | tr -d ' ')
echo "install-netsurf: $n resource files under /usr/share/netsurf"

# The fonts, at the path NETSURF_FB_FONTPATH names. NetSurf's freetype
# font layer looks for DejaVuSans.ttf and its siblings by bare filename
# along that path, so the directory matters and the names do.
"$PUT" -r "$IMAGE" "$OUT/res/fonts" /usr/share/fonts/truetype/dejavu \
    >/dev/null || exit 1
echo "install-netsurf: DejaVu under /usr/share/fonts/truetype/dejavu"

echo "install-netsurf: done - /bin/netsurf -f leanos"
