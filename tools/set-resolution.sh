#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/.."

RES="${1:-}"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT=build/leanfs-put

[ -n "$RES" ] || { echo "usage: $0 <width>x<height>" >&2; exit 2; }

SUPPORTED="800x600 1024x768 1152x864 1280x720 1280x1024 1440x900 1600x900 1680x1050 1920x1080"

if [ "$RES" = "--check" ]; then
  DRIVER=$(awk '/CANDIDATES\[\] = \{/,/\};/' kernel/drivers/dispi.c |
           grep -o '{ *[0-9]*, *[0-9]* *}' | tr -d '{} ' | tr ',' 'x' | tr '\n' ' ' | sed 's/ $//')
  if [ "$DRIVER" != "$SUPPORTED" ]; then
    echo "set-resolution: this script accepts '$SUPPORTED'" >&2
    echo "                kernel/drivers/dispi.c offers '$DRIVER'" >&2
    exit 1
  fi
  echo "set-resolution: the same modes as kernel/drivers/dispi.c"
  exit 0
fi

W="${RES%x*}"
H="${RES#*x}"
case "$W$H" in
  *[!0-9]*|"") echo "set-resolution: '$RES' is not <width>x<height>" >&2; exit 2 ;;
esac

ok=0
for m in $SUPPORTED; do [ "$m" = "${W}x${H}" ] && ok=1; done
if [ "$ok" -ne 1 ]; then
  echo "set-resolution: ${W}x${H} is not one of the modes this driver offers." >&2
  echo "                $SUPPORTED" >&2
  exit 2
fi

[ -f "$IMAGE" ] || { echo "set-resolution: no $IMAGE - run make first" >&2; exit 1; }
[ -x "$PUT" ] || make -s leanfs-put || exit 1

TMP=$(mktemp -t leanos-settings-XXXXXX)
printf 'display_w=%s\ndisplay_h=%s\n' "$W" "$H" > "$TMP"
"$PUT" "$IMAGE" "$TMP" /etc/settings.conf >/dev/null || { rm -f "$TMP"; exit 1; }
rm -f "$TMP"
echo "set-resolution: ${W}x${H} written to /etc/settings.conf in $IMAGE"
