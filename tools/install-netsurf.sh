#!/usr/bin/env bash
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

BIN="$OUT/netsurf.stripped"
cp "$OUT/netsurf" "$BIN"
if [ -x "$CROSS_STRIP" ]; then
  "$CROSS_STRIP" "$BIN" 2>/dev/null || true
fi

"$PUT" "$IMAGE" "$BIN" /bin/netsurf >/dev/null || exit 1
before=$(wc -c < "$OUT/netsurf")
after=$(wc -c < "$BIN")
echo "install-netsurf: /bin/netsurf ($((after / 1024))K, was $((before / 1024))K unstripped)"

"$PUT" -r "$IMAGE" "$OUT/res" /usr/share/netsurf >/dev/null || exit 1
n=$(find "$OUT/res" -type f | wc -l | tr -d ' ')
echo "install-netsurf: $n resource files under /usr/share/netsurf"

"$PUT" "$IMAGE" tools/netsurf-port/flex.html /usr/share/netsurf/flex.html >/dev/null || exit 1

"$PUT" -r "$IMAGE" "$OUT/res/fonts" /usr/share/fonts/truetype/dejavu \
    >/dev/null || exit 1
echo "install-netsurf: DejaVu under /usr/share/fonts/truetype/dejavu"

CA_PKG=$(ls build/repo/ca-certificates-*.osp 2>/dev/null | head -1)
if [ -n "$CA_PKG" ] && [ -f build/repo/index ]; then
  make -s packages >/dev/null || exit 1
  LIST=$(mktemp -t leanos-preinstall-XXXXXX)
  printf 'ca-certificates\n' > "$LIST"
  "$PUT" "$IMAGE" "$LIST" /pkg/repo/preinstall >/dev/null || { rm -f "$LIST"; exit 1; }
  rm -f "$LIST"
  echo "install-netsurf: ca-certificates in /pkg/repo, installed by \`os preinstall\` on first boot"
else
  echo "install-netsurf: no ca-certificates package in build/repo - https will fail"
  echo "                 certificate checks. tools/build-packages.sh ca-certificates builds it."
fi

echo "install-netsurf: done - /bin/netsurf -f leanos"
