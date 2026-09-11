#!/usr/bin/env bash
# tools/set-resolution.sh - M114: the screen size, from outside the image.
#
# The desktop can already change resolution live (Settings, with a
# countdown that puts it back if you do not confirm) and remembers the
# choice in /etc/settings.conf. What it cannot survive is a kernel
# rebuild: $(IMAGE)'s recipe recreates the disk, leanfs goes with it, and
# the machine comes back at the firmware's 1024x768 with the setting
# gone. That is the same wipe M113 found under the browser, and this is
# the same answer - re-apply it from outside, every run.
#
#   QEMU_RES=1440x900 ./tools/run-qemu.sh
#
# Why not simply raise the compiled-in default: every coordinate in
# tools/qemu_input_suite.py is measured against a 1024x768 framebuffer,
# so changing the default silently invalidates fifty interactive tests.
# The default stays where the tests are; this is how a person gets a
# bigger screen.
#
# The size is checked against kernel/drivers/dispi.c's CANDIDATES here
# rather than being passed through and refused at boot, because a
# rejected mode leaves the compositor on the firmware's mode with no
# message anybody sees.
set -uo pipefail
cd "$(dirname "$0")/.."

RES="${1:-}"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT=build/leanfs-put

[ -n "$RES" ] || { echo "usage: $0 <width>x<height>" >&2; exit 2; }

# kernel/drivers/dispi.c's CANDIDATES, which is the list the driver will
# actually offer. Kept in step by hand and checked by `--check` below,
# because a list that drifts is a mode this script accepts and the
# machine refuses.
SUPPORTED="800x600 1024x768 1152x864 1280x720 1280x1024 1440x900 1600x900 1680x1050 1920x1080"

# M116: `--check` is the check this header promised and nothing ran -
# the sentence above said run-tests.sh compared the two lists, and it did
# not. Now it does, in the fast tier.
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

# Read-modify-write would need a host-side leanfs reader; there is none,
# and settings.conf's own loader treats every key as optional (see
# user_space/lib/settings_file.c) - a file with only the display keys in
# it means "this size, and no opinion about anything else", which is
# exactly right for a size supplied from outside. The colours and volume
# a person set in Settings are the cost, and they are recorded here
# rather than hidden: this is a switch for the screen, not a settings
# editor.
TMP=$(mktemp -t leanos-settings-XXXXXX)
printf 'display_w=%s\ndisplay_h=%s\n' "$W" "$H" > "$TMP"
"$PUT" "$IMAGE" "$TMP" /etc/settings.conf >/dev/null || { rm -f "$TMP"; exit 1; }
rm -f "$TMP"
echo "set-resolution: ${W}x${H} written to /etc/settings.conf in $IMAGE"
