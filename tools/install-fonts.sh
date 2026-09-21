#!/usr/bin/env bash
set -uo pipefail

# The fonts, and the file that says where they are.
#
# They used to arrive as part of the browser: tools/install-netsurf.sh put
# DejaVu under /usr/share/fonts/truetype/dejavu because NetSurf's freetype
# needed it, and nothing else on this machine asked. That made the fonts a
# property of one program rather than of the machine - and the next browser
# asks a different question, through fontconfig, which looks for a
# configuration file this image did not have at all.
#
# So fonts are their own step. The family is DejaVu because it is what this
# project already fetches (tools/build-thirdparty.sh), it is unmodified, and
# it covers Latin, Greek and Cyrillic in four styles.

cd "$(dirname "$0")/.."
ROOT=$(pwd)

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT="$ROOT/build/leanfs-put"

[ -f "$IMAGE" ] || { echo "install-fonts: no image at $IMAGE - run make first" >&2; exit 1; }
make -s -C "$ROOT" leanfs-put > /dev/null 2>&1

TTF=$(echo "$ROOT"/build/thirdparty-src/dejavu-fonts-ttf-*/ttf | head -1)
if [ ! -d "$TTF" ]; then
  echo "install-fonts: no DejaVu - run tools/build-thirdparty.sh" >&2
  exit 1
fi

STAGE=$(mktemp -d)
mkdir -p "$STAGE/dejavu"
for f in DejaVuSans DejaVuSans-Bold DejaVuSans-Oblique DejaVuSans-BoldOblique \
         DejaVuSerif DejaVuSerif-Bold DejaVuSansMono DejaVuSansMono-Bold; do
  cp "$TTF/$f.ttf" "$STAGE/dejavu/" || exit 1
done

"$PUT" -r "$IMAGE" "$STAGE/dejavu" /usr/share/fonts/truetype/dejavu > /dev/null || exit 1
"$PUT" "$IMAGE" "$ROOT/tools/fonts/fonts.conf" /etc/fonts/fonts.conf > /dev/null || exit 1
rm -rf "$STAGE"

echo "install-fonts: DejaVu under /usr/share/fonts/truetype/dejavu, and"
echo "install-fonts: /etc/fonts/fonts.conf, which is where fontconfig looks"
