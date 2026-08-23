#!/usr/bin/env bash
# M40's interactive-input regression check - the click/drag/keyboard
# counterpart to tools/qemu-serial-test.sh's boot-marker check.
#
# qemu-serial-test.sh boots the image and grades the serial log: it proves
# every boot-time self-test still passes. What it cannot see is anything
# that only happens when a human touches the machine, and until M40 that
# was every single interactive feature this project has - each one covered
# only by a self-test that called the underlying action directly
# (apply_window_action / a hand-written WM_ACTION_PIPE request), never by a
# real click at a real pixel coordinate. That gap eventually hid a real
# bug: the third app launched from a desktop icon silently never opened,
# and every protocol-level self-test kept passing throughout. See
# milestones.md's M40 section.
#
# This script drives real `sendkey`/`mouse_move`/`mouse_button` commands
# through QEMU's own HMP monitor and grades real framebuffer pixels read
# back with `screendump`. The guest cannot tell this input from a human's.
#
# Usage:
#   tools/qemu-input-test.sh                 # every test
#   tools/qemu-input-test.sh alt_tab_cycles_focus [more...]
#
# Each test boots its own guest, so a full run takes a few minutes. Run it
# alongside qemu-serial-test.sh before every milestone from here on -
# neither one subsumes the other.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f build/os-image.bin ]; then
  echo "No image at build/os-image.bin yet - run 'make' first." >&2
  exit 1
fi

exec python3 tools/qemu_input_suite.py "$@"
