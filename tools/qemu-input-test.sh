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
#   tools/qemu-input-test.sh --quick         # the pre-commit subset
#   tools/qemu-input-test.sh --jobs 2        # fewer guests at once
#   tools/qemu-input-test.sh --no-snapshot   # boot every guest cold (Q19's baseline)
#   tools/qemu-input-test.sh --check-stale   # prove a stale snapshot is refused
#   tools/qemu-input-test.sh alt_tab_cycles_focus [more...]
#
# Q19: each test gets its own guest, which is what makes them
# independent - but that guest is RESTORED from a snapshot of a painted
# desktop rather than booted. The boot happens once per image. Several
# guests still run at once (a third of the machine's cores, capped at
# four - see default_jobs).
#
# Four tests still boot cold, because they are about the boot: see
# COLD_BOOT_TESTS in qemu_input_suite.py. The snapshot is keyed on a hash
# of the image, so a rebuilt kernel cannot be tested against yesterday's
# snapshot; `--check-stale` proves that rather than asserting it.
#
# Q1: a full run is about eight minutes - 47 tests in 458 s, measured.
# This comment said "about twenty minutes" for several milestones and was
# right when it was written; what changed is that the boot self-tests
# stopped running on every boot (kernel/dev/fwcfg.h), which took the boot
# from ~190 s to ~8 and took roughly two thirds off every test here as a
# side effect. Re-measured rather than adjusted by reasoning, and worth
# keeping honest: a stale number in this header is how somebody decides
# not to run this.
#
# `--quick` is nine tests covering the most ground per minute, for the
# "did I break the desktop" question you want answered before a commit
# rather than after.
#
# Run the full one alongside qemu-serial-test.sh before every milestone -
# neither one subsumes the other.
#
# A caveat worth knowing before reading a failure: the guests compete for
# the same cores, and the failure that competition produces is a *boot
# timeout*, which is this harness giving up rather than a verdict about
# the desktop. The timeout scales with the job count for that reason. If
# a run reports one, re-run that test on its own before believing it.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f build/os-image.bin ]; then
  echo "No image at build/os-image.bin yet - run 'make' first." >&2
  exit 1
fi

exec python3 tools/qemu_input_suite.py "$@"
