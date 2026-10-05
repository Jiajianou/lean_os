#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/.."

SECONDS_TO_RUN="${1:-240}"
CORES="${2:-4}"

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS="build/ovmf/OVMF_VARS.fd"
for f in "$IMAGE" "$OVMF_CODE" "$OVMF_VARS"; do
  [ -f "$f" ] || { echo "missing $f - run 'make' first" >&2; exit 1; }
done

WORK="$(mktemp -d -t leanos-smp-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
LOG="${LEANOS_SERIAL_LOG:-$WORK/serial.log}"
: > "$LOG"
cp "$OVMF_VARS" "$WORK/vars.fd"

qemu-system-x86_64 \
  -m "${QEMU_MEM:-4096}" -smp "$CORES" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$WORK/vars.fd" \
  -drive if=none,id=disk0,format=raw,snapshot=on,file="$IMAGE" \
  -device virtio-blk-pci,drive=disk0 \
  -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  -serial file:"$LOG" -monitor none &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

FINAL="[m106] cores this machine can use:"
deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
outcome="timeout"
while [ "$(date +%s)" -lt "$deadline" ]; do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then outcome="qemu exited"; break; fi
  if grep -qF "$FINAL" "$LOG" 2>/dev/null; then outcome="reached the measurement"; break; fi
  if grep -qF "KERNEL PANIC" "$LOG" 2>/dev/null; then outcome="panicked"; break; fi
  sleep 1
done
kill -9 "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

fail=0
if grep -qF "KERNEL PANIC" "$LOG"; then
  echo "FAIL: the machine panicked on $CORES cores:"
  grep -aF "KERNEL PANIC" "$LOG" | head -1
  fail=1
fi

# [logwrite] is here because one core cannot fail it: several programs write
# whole lines at once and the kernel reads every one back out of its log. On
# a lock taken per character - sys_write until it staged a write - four cores
# tore 15 of 480 lines, and eight tore a battery marker.
#
# M225: every [logwrite] line is one kernel_log_write, so it is contiguous
# wherever it lands - but a kernel line built from several calls can still
# be split around it, and nothing here is anchored at a line's start for
# that reason. The self-test grades itself (it panics on a write that is
# not in the log whole exactly once, or on an interrupts-off section that
# handed the serial port and console more than KERNEL_LOG_DRAIN_BATCH
# characters); these lines are its numbers, shown.
grep -ao '\[logwrite\] \(interrupts off\|per character\|forktest\|the first\)[[:print:]]*' "$LOG" | head -4

want_online=$(printf '[smp] %08X CPU(s) online.' "$CORES")
for m in "$want_online" \
         "[smp] self-test passed." \
         "[logwrite] one write() is one piece of the log:" \
         "[m106] cores this machine can use:"; do
  if grep -qF "$m" "$LOG"; then
    echo "ok    $m"
  else
    echo "MISS  $m"
    fail=1
  fi
done

cost=$(grep -ao '\[perf\] smp_parallel_cost_pct [0-9][0-9]*' "$LOG" | tail -1 | awk '{print $3}')
if [ -z "${cost:-}" ]; then
  echo "MISS  a parallel-cost measurement"
  fail=1
else
  ceiling=$(( CORES * 75 ))
  if [ "$cost" -gt "$ceiling" ]; then
    echo "FAIL  $CORES times the work cost ${cost}% of one task, over the ${ceiling}% ceiling -"
    echo "      these cores are not sharing the work. See M106."
    fail=1
  else
    echo "ok    $CORES times the work cost ${cost}% of one task (ceiling ${ceiling}%, 100 means it went wide)"
  fi
fi

grep -ao '\[m106\][[:print:]]*' "$LOG" | head -2
echo "($outcome)"
[ "$fail" = 0 ] && echo "PASS - $CORES cores start, know themselves, and share the work." || echo "FAIL"
exit "$fail"
