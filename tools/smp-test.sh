#!/usr/bin/env bash
# tools/smp-test.sh - M106: boot this kernel on four cores and grade the
# part of it that works.
#
# ---- why this is a separate harness ----------------------------------
#
# Every graded boot before M106 ran on one core. Nothing in this tree
# ever passed `-smp` and QEMU's default is 1, so a kernel that has had an
# AP trampoline, per-CPU GDTs and TSSes, a scheduler-tick IPI and per-CPU
# run state since M7 had never started a second core in a test. Its own
# [smp] self-test printed "1 CPU(s) online" on every run and passed.
#
# The first four-core boot triple-faulted before the first AP reached any
# C at all. Five real bugs later (see milestones.md M106) it gets most of
# the way through the battery, and the rest is named there rather than
# hidden: the TCP self-test's bulk transfer stalls, and one animation
# frame misses its budget. Until those are fixed, defaulting
# tools/qemu-serial-test.sh to four cores would make every run of it red,
# and defaulting it to one would let the multi-core path rot again
# exactly the way it just did.
#
# So: a short boot on four cores that grades the things that ARE true.
# Every core starts, every core recognises itself, every core is sampled
# by the profiler, and four tasks of equal work take about as long as one
# - which is the measurement M106's remaining bullets (per-CPU run
# queues, work stealing) would have to be justified by.
#
# Usage:  tools/smp-test.sh [seconds] [cores]
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
LOG="$WORK/serial.log"
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

# The last marker this harness needs, not the last one the boot prints -
# it stops as soon as it has what it grades rather than sitting through
# the rest of the battery, which is what qemu-serial-test.sh is for.
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

# Every core online. The count is the point: "some cores started" is what
# this kernel silently did for four milestones.
want_online=$(printf '[smp] %08X CPU(s) online.' "$CORES")
for m in "$want_online" \
         "[smp] self-test passed." \
         "[m106] cores this machine can use:"; do
  if grep -qF "$m" "$LOG"; then
    echo "ok    $m"
  else
    echo "MISS  $m"
    fail=1
  fi
done

# The measurement, and a ceiling over it. What the number is here to
# catch is a scheduler that serialises, which reads as N*100; the ceiling
# is deliberately loose of 100 because this is QEMU's multi-threaded TCG
# on a host with its own opinions about scheduling.
cost=$(grep -a '^\[perf\] smp_parallel_cost_pct ' "$LOG" | tail -1 | awk '{print $3}')
if [ -z "${cost:-}" ]; then
  echo "MISS  a parallel-cost measurement"
  fail=1
else
  # Three-quarters of fully-serialised. Measured at 129%, 200% and 155%
  # across three runs on a quiet host - a wide spread, because QEMU's
  # vCPU threads are at the mercy of the host's own scheduler, and the
  # kernel already takes the best of three rounds inside the guest. What
  # this catches is the shape that means the cores are not being used at
  # all, which is 400 on four cores, not the difference between 130 and
  # 200. (An hour of that spread turned out to be a QEMU process left
  # running by an aborted earlier run, which is worth knowing before
  # reading anything into a single number here.)
  ceiling=$(( CORES * 75 ))
  if [ "$cost" -gt "$ceiling" ]; then
    echo "FAIL  $CORES times the work cost ${cost}% of one task, over the ${ceiling}% ceiling -"
    echo "      these cores are not sharing the work. See M106."
    fail=1
  else
    echo "ok    $CORES times the work cost ${cost}% of one task (ceiling ${ceiling}%, 100 means it went wide)"
  fi
fi

grep -a '^\[m106\]' "$LOG" | head -2
echo "($outcome)"
[ "$fail" = 0 ] && echo "PASS - $CORES cores start, know themselves, and share the work." || echo "FAIL"
exit "$fail"
