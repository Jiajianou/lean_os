#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_PYBUILD_LOG:-build/pybuild-serial.log}"
SECONDS_TO_RUN="${1:-900}"
PY_VER=3.12.7
SRC="$ROOT/build/toolchain-src/Python-$PY_VER"

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
[ -f "$OVMF_CODE" ] || ./tools/build-ovmf.sh || exit 1
[ -f "$IMAGE" ] || { echo "python-build-test: no image at $IMAGE - run make first" >&2; exit 1; }

OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_pybuild.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

if [ "${QEMU_DISK:-virtio}" = "ide" ]; then
  DISK_ARGS=(-drive "format=raw,snapshot=on,file=$IMAGE")
else
  DISK_ARGS=(-drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE"
             -device virtio-blk-pci,drive=disk0)
fi

QEMU_MEM=${QEMU_MEM:-2048}
QEMU_CPUS=${QEMU_CPUS:-1}

echo "[harness] booting for the build measurement - ceiling ${SECONDS_TO_RUN}s, ${QEMU_CPUS} cpu(s), ${QEMU_MEM} MiB"

qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  "${DISK_ARGS[@]}" -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  -fw_cfg name=opt/leanos/pybuild,string=1 \
  -serial file:"$LOG" -monitor none &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

DONE_MARKER="[init] PID 1 spawned"
deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
outcome="timeout"
while [ "$(date +%s)" -lt "$deadline" ]; do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then outcome="qemu exited"; break; fi
  if grep -qF "$DONE_MARKER" "$LOG" 2>/dev/null; then
    outcome="the measurement finished and the machine went on booting"; sleep 1; break
  fi
  if grep -qF "*** KERNEL PANIC:" "$LOG" 2>/dev/null; then
    outcome="kernel panic"; sleep 1; break
  fi
  sleep 3
done
elapsed=$(( $(date +%s) - (deadline - SECONDS_TO_RUN) ))
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo
echo "[harness] $outcome after ${elapsed}s (ceiling ${SECONDS_TO_RUN}s)."
echo

if grep -qF "[m99build] no pybuild fixture on this image" "$LOG"; then
  echo "python-build-test: the fixture is not on the image - skipped."
  exit 0
fi
if grep -qF "m99build: no /bin/gcc on this image" "$LOG"; then
  echo "python-build-test: no native compiler on the image - skipped."
  echo "                   tools/install-native-toolchain.sh puts one there."
  exit 0
fi

fail=0
need() { grep -qF "$1" "$LOG" || { echo "MISSING: $1"; fail=1; }; }
need "== m99build:"
need "m99build: done"

ms_of() {
  awk -v label="$1" '
    $0 ~ ("\\[measure\\] " label ":") {
      for (i = 1; i <= NF; i++) if ($i == "wall") { print $(i+1); exit }
    }' "$LOG"
}
rss_of() {
  awk -v label="$1" '
    $0 ~ ("\\[measure\\] " label ":") {
      for (i = 1; i <= NF; i++) if ($i == "peak-rss") { print $(i+1); exit }
    }' "$LOG"
}

PROBE_FIRST=$(ms_of probe-first)
P1=$(ms_of probe-1); P2=$(ms_of probe-2); P3=$(ms_of probe-3)
TU_MS=$(ms_of tu); TU_RSS=$(rss_of tu)

for v in PROBE_FIRST P1 P2 P3; do
  [ -n "${!v}" ] || { echo "MISSING: a wall-clock for $v"; fail=1; }
done
[ $fail -eq 0 ] || { echo; echo "FAIL - the measurement did not produce its numbers."; exit 1; }

PROBE_WARM=$(( (P1 + P2 + P3) / 3 ))

CHECKS=""
CFILES=""
CFGOUT="$ROOT/build/configure-test/ours/configure.out"
[ -f "$CFGOUT" ] && CHECKS=$(grep -c '^checking' "$CFGOUT")
PYOBJ="$ROOT/build/python/build-shared"
[ -d "$PYOBJ" ] && CFILES=$(find "$PYOBJ" -name '*.o' 2>/dev/null | wc -l | tr -d ' ')

if [ -z "$CHECKS" ] || [ "$CHECKS" = "0" ]; then
  echo "python-build-test: no configure run to count checks from."
  echo "                   tools/configure-test.sh produces one. Skipping"
  echo "                   the extrapolation rather than guessing at it."
  echo
  grep -E "^\[perf\] pybuild_" "$LOG" | sed 's/^/  /'
  echo
  echo "PASS - the units were measured; the counts to multiply them by were not available."
  exit 0
fi
if [ -z "$CFILES" ] || [ "$CFILES" = "0" ]; then
  echo "python-build-test: no cross build to count translation units from."
  echo "                   tools/build-python.sh produces one."
  CFILES=""
fi

echo "==============================================================="
echo "  M99: what CPython's own build would cost on this machine"
echo "==============================================================="
echo
echo "  MEASURED HERE, by tests/pybuild/run.sh:"
printf "    one configure probe, first run      %8s ms\n" "$PROBE_FIRST"
printf "    one configure probe, steady (mean 3)%8s ms\n" "$PROBE_WARM"
if [ -n "$TU_MS" ]; then
printf "    one C translation unit             %8s ms   peak RSS %s KiB\n" "$TU_MS" "${TU_RSS:-?}"
fi
echo
echo "  COUNTED from real runs of CPython $PY_VER's own build:"
printf "    checks its configure ran           %8s   (tools/configure-test.sh)\n" "$CHECKS"
[ -n "$CFILES" ] && \
printf "    objects its make produced          %8s   (the cross build)\n" "$CFILES"
echo
CONF_S=$(( (PROBE_WARM * CHECKS) / 1000 ))
echo "  MULTIPLIED - an argument about numbers, not an observation:"
printf "    ./configure   %s ms x %s = %s s (%s min)\n" \
       "$PROBE_WARM" "$CHECKS" "$CONF_S" "$(( CONF_S / 60 ))"
if [ -n "$TU_MS" ] && [ -n "$CFILES" ]; then
  MAKE_S=$(( (TU_MS * CFILES) / 1000 ))
  TOTAL_S=$(( CONF_S + MAKE_S ))
  printf "    make          %s ms x %s = %s s (%s min)\n" \
         "$TU_MS" "$CFILES" "$MAKE_S" "$(( MAKE_S / 60 ))"
  printf "    together                       %s s (%s h %s min)\n" \
         "$TOTAL_S" "$(( TOTAL_S / 3600 ))" "$(( (TOTAL_S % 3600) / 60 ))"
fi
echo
echo "  Every input above is measured. The product is arithmetic over"
echo "  measurements and is not a run: this machine has never built"
echo "  CPython, and the number is what says by how much."
echo
grep -E "^\[perf\] pybuild_" "$LOG" | sed 's/^/  /'
echo
echo "PASS - the measurement was taken."
exit 0
