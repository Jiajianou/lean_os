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
# Usage: tools/qemu-serial-test.sh [SECONDS] [-- extra qemu args, e.g. -monitor pipe:/tmp/mon for key injection]
set -euo pipefail

# 20s: empirically enough to carry a from-scratch (unformatted-disk) boot
# through every self-test below and into the M22/M23 desktop handoff (the
# last REQUIRED_MARKERS entry) - see the M29 progress notes for the actual
# timings this was measured against. Bump if a future milestone adds
# enough boot-time work to push past it; this is a real budget, not a
# magic number to leave stale.
SECONDS_TO_RUN="${1:-24}"
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
  -drive format=raw,file="$IMAGE" -display none \
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
