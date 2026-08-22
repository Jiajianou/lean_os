#!/usr/bin/env bash
# Boots build/os-image.bin headlessly, captures COM1 (see kernel/drivers/serial.c)
# to a file for SECONDS, then prints it and exits. Used to verify kernel
# behavior from the command line without a screendump/monitor dance -
# every klog_* message (drivers/klog.h) reaches this capture.
#
# Usage: tools/qemu-serial-test.sh [SECONDS] [-- extra qemu args, e.g. -monitor pipe:/tmp/mon for key injection]
set -euo pipefail

SECONDS_TO_RUN="${1:-2}"
shift || true
EXTRA_ARGS=("$@")

IMAGE="build/os-image.bin"
LOG="$(mktemp -t qemu-serial-XXXXXX.log)"

if [ ! -f "$IMAGE" ]; then
  echo "No image at $IMAGE yet - run 'make' first." >&2
  exit 1
fi

qemu-system-x86_64 -drive format=raw,file="$IMAGE" -display none \
  -serial file:"$LOG" -monitor none ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

sleep "$SECONDS_TO_RUN"
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

cat "$LOG"
rm -f "$LOG"
