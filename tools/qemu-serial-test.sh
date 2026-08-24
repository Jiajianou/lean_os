#!/usr/bin/env bash
# Boots build/os-image.bin headlessly via OVMF (M26: UEFI-only, BIOS boot
# path removed), captures COM1 (see kernel/drivers/serial.c) to a file for
# SECONDS, then grades the capture as a real pass/fail regression check -
# every klog_* message (drivers/klog.h) reaches this capture, including
# every boot-time self-test's own "X self-test passed" line.
#
# M29: this used to just dump the log and exit 0 unconditionally, leaving
# "did anything actually break" a read-the-log-and-eyeball-it job for a
# human - fine when this was the only milestone or two deep, not once
# growing UX/app complexity means a regression in, say, M14's pipe
# self-test could scroll by unnoticed under a screenful of M22 GUI output.
# Now it fails loudly (nonzero exit, a summary of exactly what's missing)
# if the kernel panicked or any REQUIRED_MARKERS entry never showed up -
# meant to run before every milestone from here on, not just when
# something looks wrong.
#
# M40: `snapshot=on` on the disk. Guest writes (leanfs formats the disk on
# its first boot) go to a throwaway overlay instead of back into
# build/os-image.bin, which fixes two things at once: this script no longer
# takes a write lock on the image - so it can run alongside
# tools/qemu-input-test.sh or a `make` - and every run is genuinely the
# from-scratch, unformatted-disk boot SECONDS_TO_RUN's budget below is
# written against. Before this, only the first run after a rebuild was;
# every one after that booted the already-formatted disk the previous run
# left behind, quietly skipping the format path it claims to cover.
#
# M40: this remains the *boot-time* check only. Everything a human has to
# click to reach now has its own harness - tools/qemu-input-test.sh, which
# injects real mouse/keyboard events and grades real framebuffer pixels.
# Run both before every milestone; neither subsumes the other (see that
# script's header for what this one structurally cannot see).
#
# Usage: tools/qemu-serial-test.sh [SECONDS] [-- extra qemu args, e.g. -monitor pipe:/tmp/mon for key injection]
set -euo pipefail

# Enough to carry a from-scratch (unformatted-disk) boot through every
# self-test below and into the desktop handoff (the last REQUIRED_MARKERS
# entry) - see the M29 progress notes for the timings the original 24s was
# measured against. A real budget, not a magic number to leave stale: bump
# it when a milestone adds enough boot-time work to push past it, which
# M40 (two new self-tests) and M41 (a fourth desktop client, and every
# self-test's own compositor now opening four more pipes) between them
# did - 32s was measured failing and 34s passing, so 40 leaves real
# headroom rather than sitting one slow boot away from a false failure.
#
# M42/M43 both added a self-test and the boot got *faster*: 30s now passes
# comfortably. M42's selftest_reap is why - every self-test now waits for
# the clients it kills instead of leaving them to be noticed some
# arbitrary number of scheduler quanta later, and a killed compositor that
# keeps getting scheduled is not free. The budget stayed at 40 through M44;
# the headroom is the point.
#
# M45: 40 -> 50. The new self-test spawns two compositor clients and waits
# out a deliberate "did the polite close *fail*?" interval, which cannot be
# shortened - proving something did not happen takes real time. 44s was
# measured passing, so 50 keeps the same "not one slow boot from a false
# failure" margin the number has always been chosen for.
SECONDS_TO_RUN="${1:-50}"
shift || true
EXTRA_ARGS=("$@")

IMAGE="build/os-image.bin"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
OVMF_VARS_RUNTIME="$(mktemp -t qemu-serial-ovmf-vars-XXXXXX.fd)"
LOG="$(mktemp -t qemu-serial-XXXXXX.log)"

if [ ! -f "$IMAGE" ]; then
  echo "No image at $IMAGE yet - run 'make' first." >&2
  exit 1
fi

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
  echo "No OVMF firmware at build/ovmf/ yet - building it now from source" >&2
  echo "(tools/build-ovmf.sh; one-time, several minutes)..." >&2
  ./tools/build-ovmf.sh
fi

cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS_RUNTIME"

# -netdev user: see tools/run-qemu.sh's comment - needed here too since
# the boot self-test (kernel/kernel.c) pings the gateway and would panic
# without a NIC attached at all.
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive format=raw,snapshot=on,file="$IMAGE" -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -serial file:"$LOG" -monitor none ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

sleep "$SECONDS_TO_RUN"
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

cat "$LOG"

# M29: every boot-time self-test's own "passed"/"verified" klog line -
# kept as literal substrings of kernel/kernel.c's own klog_puts calls
# (not regexes) so a wording tweak there is a visible one-line diff here
# too, rather than a silently-still-passing check that stopped meaning
# what its neighbors say it means. Order matches boot order. The last
# entry (init handoff) isn't itself a self-test - it's the "boot actually
# reached steady state" checkpoint everything above is a prerequisite for.
REQUIRED_MARKERS=(
  "[vmm] map/unmap self-test passed."
  "[heap] kmalloc/kfree self-test passed."
  "[fb] framebuffer clear/fill/readback self-test passed."
  "[font39] glyph table + shared-baseline render self-test passed."
  "[sched] back on the main task - preemption round trip verified."
  "[syscall] SYS_exit self-test task ran and terminated."
  "[pipe] kernel-level producer/consumer self-test passed."
  "[pipe] SYS_pipe/SYS_write/SYS_read self-test passed."
  "[signal] SIGTERM self-test passed"
  "[wait] SYS_wait(-1) self-test passed"
  "[pgid] SYS_getpgid self-test passed"
  "[fs] leanfs indirect-block self-test passed"
  "[memtest] user-space malloc/free and cross-process shm self-tests passed."
  "[wm] compositor + client self-test passed"
  "[wm21] multi-window compositor + focus-routing self-test passed"
  "[wm22] desktop shell (panel + taskbar query, no launcher) self-test passed"
  "[wm30] window chrome (maximize/restore/minimize/close via WM_ACTION_PIPE) self-test passed"
  "[clipboard] SYS_clipboard_set/get self-test passed."
  "[vfs] SYS_writefile/SYS_readfile self-test passed."
  "[settings] WM_SETTINGS_PIPE background-color self-test passed."
  "[wm36] confirm_close opt-in (WM_EVENT_CLOSE_REQUEST via WM_ACTION_PIPE) self-test passed"
  "[wm38] drop shadow + WM_SETTINGS_PIPE accent-color self-test passed"
  "[m42] bottom taskbar (Start button, running-app button, tray, maximize clamp, launcher toggle) self-test passed"
  "[m43] window snapping (left/right half, buffer-clamped) and the launcher overlay self-test passed"
  "[m44] wallpaper gradient, taskbar translucency over it, and the settings query round trip self-test passed"
  "[m45] SYS_taskinfo naming, WM_ACTION_KILL forcing a confirm_close client"
  "[m40] boot-task fd reset self-test passed"
  "[m40] SYS_spawn failure-path self-test passed"
  "[smp] self-test passed."
  "[net] ICMP echo request/reply self-test passed"
  "[init] PID 1 spawned"
)

pass=1

if grep -qF "*** KERNEL PANIC:" "$LOG"; then
  pass=0
  echo "FAIL: kernel panicked - $(grep -F '*** KERNEL PANIC:' "$LOG" | head -1)"
fi

missing=()
for marker in "${REQUIRED_MARKERS[@]}"; do
  if ! grep -qF "$marker" "$LOG"; then
    missing+=("$marker")
  fi
done

if [ "${#missing[@]}" -gt 0 ]; then
  pass=0
  echo "FAIL: ${#missing[@]}/${#REQUIRED_MARKERS[@]} required boot markers never appeared (log capture ended too early, or a real regression - try a longer SECONDS first):"
  for marker in "${missing[@]}"; do
    echo "  - $marker"
  done
fi

rm -f "$LOG" "$OVMF_VARS_RUNTIME"

if [ "$pass" -eq 1 ]; then
  echo "PASS: ${#REQUIRED_MARKERS[@]}/${#REQUIRED_MARKERS[@]} required boot markers found, no kernel panic."
  exit 0
fi
exit 1
