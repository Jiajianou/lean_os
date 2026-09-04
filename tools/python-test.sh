#!/usr/bin/env bash
# tools/python-test.sh - M99's fourth bullet: CPython's own suite, here.
#
# Boots the real image with `opt/leanos/pytest=1` and NOT
# `opt/leanos/selftest`, so the machine comes up in seconds and then
# spends its time running somebody else's test suite over somebody
# else's language. tests/python/run.sh is what runs; this passes the
# switch, waits, and reads the counts back out of the log.
#
# ---- what it grades, and what it deliberately does not ---------------
#
# It does NOT require every module to pass. M99's own bullet says the
# pass/fail counts are "recorded rather than summarized", and a harness
# that failed the run on the first failing assertion in somebody else's
# suite would be a harness that gets disabled the first time it is
# right. What it requires is that the report is REAL:
#
#   - the interpreter started and said which version it is
#   - every module in tests/python/run.sh's list was reached (a run cut
#     short is not a run with fewer tests in it)
#   - CPython's own runner printed a result line for each of them, so
#     the numbers below were written by it and not inferred here
#   - the script reached its own last line
#   - the machine did not panic
#
# Then it prints the totals, and `--require-clean` is there for the day
# the honest expectation is zero failures. It is not passed by default,
# and the reason is written in milestones.md next to the first run's
# numbers rather than here.
#
# Usage: tools/python-test.sh [SECONDS] [--require-clean]
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_PYTEST_LOG:-build/pytest-serial.log}"
SECONDS_TO_RUN=7200
REQUIRE_CLEAN=0
for a in "$@"; do
  case "$a" in
    --require-clean) REQUIRE_CLEAN=1 ;;
    ''|*[!0-9]*) ;;
    *) SECONDS_TO_RUN="$a" ;;
  esac
done

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
[ -f "$OVMF_CODE" ] || ./tools/build-ovmf.sh || exit 1
[ -f "$IMAGE" ] || { echo "python-test: no image at $IMAGE - run make first" >&2; exit 1; }

OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_pytest.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

if [ "${QEMU_DISK:-virtio}" = "ide" ]; then
  DISK_ARGS=(-drive "format=raw,snapshot=on,file=$IMAGE")
else
  DISK_ARGS=(-drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE"
             -device virtio-blk-pci,drive=disk0)
fi

# One core, and 2 GiB. The core count is one for the reason
# tools/bootstrap-test.sh gives at length - the battery is green on one
# core and not yet on more - and the memory is what a test suite that
# forks subprocesses and allocates freely wants without making the
# measurement about swap, which M102 already refused.
QEMU_MEM=${QEMU_MEM:-2048}
QEMU_CPUS=${QEMU_CPUS:-1}

echo "[harness] booting for the regression suite - ceiling ${SECONDS_TO_RUN}s, ${QEMU_CPUS} cpu(s), ${QEMU_MEM} MiB"

qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  "${DISK_ARGS[@]}" -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  -fw_cfg name=opt/leanos/pytest,string=1 \
  -serial file:"$LOG" -monitor none &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

DONE_MARKER="[init] PID 1 spawned"
deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
outcome="timeout"
while [ "$(date +%s)" -lt "$deadline" ]; do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then outcome="qemu exited"; break; fi
  if grep -qF "$DONE_MARKER" "$LOG" 2>/dev/null; then
    outcome="the suite finished and the machine went on booting"; sleep 1; break
  fi
  if grep -qF "*** KERNEL PANIC:" "$LOG" 2>/dev/null; then
    outcome="kernel panic"; sleep 1; break
  fi
  sleep 5
done
elapsed=$(( $(date +%s) - (deadline - SECONDS_TO_RUN) ))
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo
echo "[harness] $outcome after ${elapsed}s (ceiling ${SECONDS_TO_RUN}s)."
echo

fail=0
say_missing() { echo "MISSING: $1"; fail=1; }

if grep -qF "[m99pytest] no python fixtures on this image" "$LOG"; then
  echo "python-test: this image has no python - skipped."
  echo "          tools/build-python.sh builds it and"
  echo "          tools/install-python.sh installs it and its test suite."
  exit 0
fi

grep -qF "*** KERNEL PANIC:" "$LOG" && { echo "PANIC during the suite boot."; fail=1; }
grep -qF "m99pytest: interpreter" "$LOG" || \
  say_missing "the interpreter's own version line - it did not start"
grep -qF "== m99pytest done ==" "$LOG" || \
  say_missing "the script's own last line - the run did not finish"

# Every module in the list was reached. The list is read from the
# fixture rather than repeated here, so the two cannot drift.
MODULES=$(sed -n '/^MODULES=/,/"$/p' tests/python/run.sh | \
          sed 's/^MODULES="//; s/"$//' | tr -s ' \n' '\n' | grep -E '^test_')
nmod=0
for m in $MODULES; do
  nmod=$((nmod + 1))
  grep -qF "== m99pytest module: $m ==" "$LOG" || \
    say_missing "module $m was never reached"
  grep -qE "^== m99pytest exit $m: " "$LOG" || \
    say_missing "module $m produced no exit status - it did not come back"
done

# ---- the numbers, as CPython's own runner wrote them ------------------
#
# `Total tests: run=N failures=F errors=E skipped=S` is regrtest's line,
# printed once per module. Nothing here decides what a test is or whether
# it passed - this adds up what it said.
echo "What CPython's own test runner reported, module by module:"
printf '  %-20s %8s %8s %8s %8s\n' module run failures errors skipped
total_run=0; total_fail=0; total_err=0; total_skip=0; reported=0
for m in $MODULES; do
  line=$(awk -v start="== m99pytest module: $m ==" -v stop="== m99pytest exit $m:" '
      index($0, start) {inblock=1}
      inblock && /^Total tests: run=/ {print; }
      inblock && index($0, stop) {inblock=0}' "$LOG" | tail -1)
  if [ -z "$line" ]; then
    printf '  %-20s %8s\n' "$m" "no result line"
    say_missing "a 'Total tests:' line from CPython's runner for $m"
    continue
  fi
  reported=$((reported + 1))
  r=$(printf '%s' "$line" | sed -n 's/.*run=\([0-9]*\).*/\1/p')
  f=$(printf '%s' "$line" | sed -n 's/.*failures=\([0-9]*\).*/\1/p')
  e=$(printf '%s' "$line" | sed -n 's/.*errors=\([0-9]*\).*/\1/p')
  s=$(printf '%s' "$line" | sed -n 's/.*skipped=\([0-9]*\).*/\1/p')
  printf '  %-20s %8s %8s %8s %8s\n' "$m" "${r:-0}" "${f:-0}" "${e:-0}" "${s:-0}"
  total_run=$((total_run + ${r:-0}))
  total_fail=$((total_fail + ${f:-0}))
  total_err=$((total_err + ${e:-0}))
  total_skip=$((total_skip + ${s:-0}))
done
echo
printf '  %-20s %8s %8s %8s %8s\n' "TOTAL ($reported/$nmod)" \
       "$total_run" "$total_fail" "$total_err" "$total_skip"
echo

grep -E "^\[perf\] pytest_" "$LOG" | sed 's/^/  /'
echo

if [ "$fail" -ne 0 ]; then
  echo "FAIL - the report is not a report: something above was missing."
  exit 1
fi
if [ "$REQUIRE_CLEAN" -eq 1 ] && \
   { [ "$total_fail" -ne 0 ] || [ "$total_err" -ne 0 ]; }; then
  echo "FAIL - --require-clean was asked for and the suite is not clean."
  exit 1
fi

echo "PASS - CPython's own regression suite ran on this machine over"
echo "       $reported modules and $total_run tests, and reported"
echo "       $total_fail failures and $total_err errors in its own words."
echo "       That is a report rather than a grade; milestones.md M99 is"
echo "       where the numbers and what they mean are written down."
