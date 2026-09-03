#!/usr/bin/env bash
# tools/bootstrap-test.sh - M98's fourth box: the build, measured.
#
# Boots the real image with `opt/leanos/bootstrap=1` and NOT
# `opt/leanos/selftest`, so the machine comes up in seconds and then
# spends its time doing the one thing this harness is about: building
# somebody else's program with the toolchain that lives on its own disk,
# and reporting what that cost.
#
# What comes back on the serial line, and what each number answers:
#
#   [measure] cxx-tu ... peak-rss N KiB   M98's "peak RSS of the largest
#                                         translation unit", and the
#                                         number M102's swap decision
#                                         has been waiting for
#   [measure] bzip2-j1 / -j4 ... wall     what a real build costs here,
#                                         serially and four ways at once
#   [perf] build_peak_live_tasks          M98's third box: how close
#   [perf] build_peak_fds_one_task        `make -j` came to MAX_TASKS
#                                         and MAX_FDS
#   identical: <n objects>                whether the compiler that runs
#                                         here produces the same bytes
#                                         as the compiler that built it
#
# ---- why this is not part of the self-test battery --------------------
#
# Because it is a measurement rather than a test, it takes minutes, and
# it wants a quiet machine. The battery's own boot spawns two hundred
# processes and ends with a desktop; a build timed against that is a
# build timed against a compositor. So it gets its own fw_cfg switch and
# its own boot, exactly as Q1's argument for the self-test switch says:
# the image is byte-identical either way.
#
# ---- what it grades ---------------------------------------------------
#
# A measurement harness that graded nothing would be a script somebody
# has to read. Four things must be true, and each is a sentence about
# the machine rather than about a number:
#
#   - the build ran to completion (the script's own last line)
#   - bzip2's OWN test suite passed on this machine, which is somebody
#     else's program grading somebody else's program
#   - every object this machine compiled is byte-identical to the one
#     the cross compiler produced from the same source
#   - the measurements were actually taken (the [measure] and [perf]
#     lines are present, not merely the marker)
#
# Usage: tools/bootstrap-test.sh [SECONDS]
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_BOOTSTRAP_LOG:-build/bootstrap-serial.log}"
# A ceiling, not an estimate. The number the first green run measured is
# recorded in milestones.md M98; this leaves it a wide margin, because a
# measurement harness that times out is a measurement nobody has.
SECONDS_TO_RUN="${1:-3600}"

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
if [ ! -f "$OVMF_CODE" ]; then
  ./tools/build-ovmf.sh || exit 1
fi
[ -f "$IMAGE" ] || { echo "bootstrap-test: no image at $IMAGE - run make first" >&2; exit 1; }

# Whether the toolchain is on the image is decided by the kernel, which
# stats the fixture and says "skipped" out loud - the same discipline
# every other native-toolchain stage uses. This script reads that line
# below rather than duplicating the check against a filesystem it would
# have to learn to read.

OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_bootstrap.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"

: > "$LOG"

if [ "${QEMU_DISK:-virtio}" = "ide" ]; then
  DISK_ARGS=(-drive "format=raw,snapshot=on,file=$IMAGE")
else
  DISK_ARGS=(-drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE"
             -device virtio-blk-pci,drive=disk0)
fi

# 4 GiB and ONE core by default, and the core count is the interesting
# one.
#
# It was four, because `make -j4` on one core measures the scheduler as
# much as the build. It is one now for the reason tools/qemu-serial-
# test.sh already gives at length: **the battery is green on one core
# and is not yet green on more.** A four-core run of this harness with
# the profiler enabled reproduced M106's own switch-away guard - `cpu 3
# is switching away from 'idle' ... while standing on` another task's
# stack - which is a bug in the list M106 left open, not a bug in this
# measurement, and a measurement harness that panics on a configuration
# the project already knows is not green is measuring nothing.
#
# `make -j4` on one core is still worth running: it is the *process and
# descriptor* pressure M98's third box asks about, and four concurrent
# compiles on one core produce exactly the same number of tasks and
# descriptors as four on four.
#
# QEMU_CPUS=4 is the way to reproduce the panic above, and is what
# should be run again the day M106's remaining work lands.
QEMU_MEM=${QEMU_MEM:-4096}
QEMU_CPUS=${QEMU_CPUS:-1}

echo "[harness] booting for the build - ceiling ${SECONDS_TO_RUN}s, ${QEMU_CPUS} cpu(s), ${QEMU_MEM} MiB"

qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  "${DISK_ARGS[@]}" -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  -fw_cfg name=opt/leanos/bootstrap,string=1 \
  -serial file:"$LOG" -monitor none &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

FINAL_MARKER="[m98boot]"
DONE_MARKER="[init] PID 1 spawned"
deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
outcome="timeout"
while [ "$(date +%s)" -lt "$deadline" ]; do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    outcome="qemu exited"
    break
  fi
  if grep -qF "$DONE_MARKER" "$LOG" 2>/dev/null; then
    outcome="the build finished and the machine went on booting"
    sleep 1
    break
  fi
  if grep -qF "*** KERNEL PANIC:" "$LOG" 2>/dev/null; then
    outcome="kernel panic"
    sleep 1
    break
  fi
  sleep 2
done
elapsed=$(( $(date +%s) - (deadline - SECONDS_TO_RUN) ))
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

cat "$LOG"
echo
echo "[harness] $outcome after ${elapsed}s (ceiling ${SECONDS_TO_RUN}s)."

fail=0
say_missing() { echo "MISSING: $1"; fail=1; }

if grep -qF "[m98boot] no build fixtures on this image" "$LOG"; then
  echo
  echo "bootstrap-test: this image has no native toolchain - skipped."
  echo "          tools/build-native-toolchain.sh builds it and"
  echo "          tools/install-native-toolchain.sh installs it."
  exit 0
fi

grep -qF "*** KERNEL PANIC:" "$LOG" && { echo "PANIC during the build boot."; fail=1; }
grep -qF "== m98boot done ==" "$LOG" || say_missing "the build script's own last line - it did not finish"
grep -qF "[m98boot] the toolchain built somebody else's program here" "$LOG" || \
  say_missing "the kernel's summary line"

# bzip2's own test suite. Its Makefile prints this exact line when every
# comparison it makes has passed - and it is somebody else's assertion
# about somebody else's program, which is the only kind this harness
# cannot have written to suit itself.
grep -qF "If you got this far and the 'cmp's didn't complain" "$LOG" || \
  say_missing "bzip2's own test suite passing on the machine"

# The measurements themselves.
for m in "cxx-tu" "c-tu" "bzip2-j1" "bzip2-j4" "bzip2-test"; do
  grep -qE "^\[measure\] $m: " "$LOG" || say_missing "the [measure] line for $m"
done
grep -qE "peak-rss [0-9]+ KiB" "$LOG" || \
  say_missing "a peak resident set - the kernel measured no pages for any child"
for p in build_wall_s build_peak_live_tasks build_peak_fds_one_task; do
  grep -qE "^\[perf\] $p " "$LOG" || say_missing "the [perf] row $p"
done

# The identity claim: eight objects, all of them equal to what the cross
# compiler produced. A differing object is not a harness failure to be
# tolerated - it means the compiler that runs here and the compiler that
# built it disagree about the same source, which is precisely what a
# bootstrap comparison exists to catch.
identical=$(grep -c "^identical: " "$LOG" || true)
differing=$(grep -c "^differs: " "$LOG" || true)
if [ "$identical" -ne 8 ] || [ "$differing" -ne 0 ]; then
  echo "OBJECT COMPARISON: $identical identical, $differing differing (expected 8 and 0)"
  grep "^differs: " "$LOG" || true
  fail=1
fi

# ---- the numbers, against their ceilings -------------------------------
#
# The same mechanism tools/qemu-serial-test.sh uses, on the same file:
# `[perf] name value unit` lines graded against tests/budgets.tsv. A
# measurement with no row is reported rather than failed, which is what
# lets a new one appear before anybody has chosen its ceiling - and a
# ceiling here is worth having for the same reason it is there: this
# build is the slowest thing this machine does, and "it got twice as
# slow" is not something anybody will notice by reading a log.
BUDGETS="tests/budgets.tsv"
perf_lines="$(grep -aoE '^\[perf\] [a-z0-9_]+ [0-9]+ [a-z]+' "$LOG" || true)"
if [ -n "$perf_lines" ]; then
  echo
  echo "Measurements this build (ceiling from $BUDGETS):"
  while read -r _tag name value unit; do
    [ -n "${name:-}" ] || continue
    row="$(awk -F'\t' -v n="$name" '$1 == n {print; exit}' "$BUDGETS" 2>/dev/null || true)"
    if [ -z "$row" ]; then
      printf '  %-34s %10s %-4s  (no budget yet)\n' "$name" "$value" "$unit"
      continue
    fi
    ceiling="$(printf '%s' "$row" | cut -f2)"
    measured="$(printf '%s' "$row" | cut -f4)"
    if [ "$value" -gt "$ceiling" ] 2>/dev/null; then
      printf '  %-34s %10s %-4s  OVER BUDGET (ceiling %s, measured %s)\n' \
        "$name" "$value" "$unit" "$ceiling" "$measured"
      fail=1
    else
      printf '  %-34s %10s %-4s  ok (ceiling %s, was %s)\n' \
        "$name" "$value" "$unit" "$ceiling" "$measured"
    fi
  done <<< "$perf_lines"
fi

echo
if [ "$fail" -ne 0 ]; then
  echo "FAIL - the build boot did not produce everything M98's fourth box asks for."
  exit 1
fi

echo "PASS - the toolchain on this machine built somebody else's program, that"
echo "       program's own test suite passed, every object it compiled is"
echo "       byte-identical with the cross compiler's, and the four"
echo "       measurements are in the log above:"
grep -E "^\[measure\] |^\[perf\] build_" "$LOG" | sed 's/^/       /'
