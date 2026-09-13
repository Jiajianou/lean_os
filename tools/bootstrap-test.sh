#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_BOOTSTRAP_LOG:-build/bootstrap-serial.log}"
SECONDS_TO_RUN="${1:-3600}"

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
if [ ! -f "$OVMF_CODE" ]; then
  ./tools/build-ovmf.sh || exit 1
fi
[ -f "$IMAGE" ] || { echo "bootstrap-test: no image at $IMAGE - run make first" >&2; exit 1; }

OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_bootstrap.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"

: > "$LOG"

if [ "${QEMU_DISK:-virtio}" = "ide" ]; then
  DISK_ARGS=(-drive "format=raw,snapshot=on,file=$IMAGE")
else
  DISK_ARGS=(-drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE"
             -device virtio-blk-pci,drive=disk0)
fi

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

grep -qF "If you got this far and the 'cmp's didn't complain" "$LOG" || \
  say_missing "bzip2's own test suite passing on the machine"

for m in "cxx-tu" "c-tu" "bzip2-j1" "bzip2-j4" "bzip2-test"; do
  grep -qE "^\[measure\] $m: " "$LOG" || say_missing "the [measure] line for $m"
done

j4_line="$(grep -E '^\[measure\] bzip2-j4: ' "$LOG" | tail -1)"
if [ -n "$j4_line" ]; then
  j4_code="$(printf '%s\n' "$j4_line" | sed -n 's/.*exit \(-\{0,1\}[0-9]\{1,\}\).*/\1/p')"
  case "$j4_code" in
    0)   echo "NOTE: make -j4 FINISHED. That has never happened before - see"
         echo "      M98's table in milestones-archive.md and the note in"
         echo "      tests/bootstrap/run.sh. Record the number and lower the -t." ;;
    124) echo "NOTE: make -j4 did not finish inside its 900 s limit, which is the"
         echo "      known outcome (M98's third box, and row 5 of the queue)."
         echo "      Bounded rather than unbounded so the rest of the run happens." ;;
    "")  say_missing "an exit status on the bzip2-j4 measure line" ;;
    *)   echo "make -j4 exited $j4_code, which is neither success nor the deadline -"
         echo "the parallel build failed rather than ran long. That is a defect,"
         echo "not the known slowness:"
         echo "  $j4_line"
         fail=1 ;;
  esac
fi
grep -qE "peak-rss [0-9]+ KiB" "$LOG" || \
  say_missing "a peak resident set - the kernel measured no pages for any child"
for p in build_wall_s build_peak_live_tasks build_peak_fds_one_task; do
  grep -qE "^\[perf\] $p " "$LOG" || say_missing "the [perf] row $p"
done

identical=$(grep -c "^identical: " "$LOG" || true)
differing=$(grep -c "^differs: " "$LOG" || true)
if [ "$identical" -ne 8 ] || [ "$differing" -ne 0 ]; then
  echo "OBJECT COMPARISON: $identical identical, $differing differing (expected 8 and 0)"
  grep "^differs: " "$LOG" || true
  fail=1
fi

BUDGETS="tests/budgets.tsv"
perf_lines="$(grep -aoE '^\[perf\] [a-z0-9_]+ [0-9]+ [a-z]+' "$LOG" || true)"
if [ -n "$perf_lines" ]; then
  echo
  echo "Measurements this build (ceiling from $BUDGETS):"
  while read -r _tag name value unit; do
    [ -n "${name:-}" ] || continue
    if [ "$name" = "boot_to_desktop_s" ]; then
      printf '  %-34s %10s %-4s  (not graded here - this boot builds before PID 1)\n' \
        "$name" "$value" "$unit"
      continue
    fi
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
