#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

RUNS="${1:-16}"
MIN_T="${2:-8.5}"
MAX_T="${3:-15.5}"

IMAGE="build/os-image.bin"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS="build/ovmf/OVMF_VARS.fd"

for f in "$IMAGE" "$OVMF_CODE" "$OVMF_VARS"; do
  [ -f "$f" ] || { echo "missing $f - run 'make' first" >&2; exit 1; }
done

WORK="$(mktemp -d -t leanos-crash-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

boot_for() {
  local img="$1" secs="$2" log="$3"
  local vars="$WORK/vars.fd"
  cp "$OVMF_VARS" "$vars"
  qemu-system-x86_64 -m 4096 -smp "${QEMU_CPUS:-1}" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$vars" \
    -drive if=none,id=d0,format=raw,file="$img" -device virtio-blk-pci,drive=d0 \
    -display none -netdev user,id=n0 -device rtl8139,netdev=n0 \
    -audiodev none,id=s0 -device AC97,audiodev=s0 \
    -serial file:"$log" -monitor none &
  local pid=$!
  perl -e "select undef, undef, undef, $secs" 2>/dev/null || sleep "${secs%.*}"
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

pass=0
fail=0
echo "Q17: $RUNS power cuts between ${MIN_T}s and ${MAX_T}s into a first boot."
echo

for i in $(seq 1 "$RUNS"); do
  t=$(awk -v i="$i" -v n="$RUNS" -v a="$MIN_T" -v b="$MAX_T" \
          'BEGIN { printf "%.2f", a + (b - a) * (i - 1) / (n > 1 ? n - 1 : 1) }')

  img="$WORK/crash-$i.bin"
  cp "$IMAGE" "$img"

  printf "  %2d/%2d  cut at %5ss  " "$i" "$RUNS" "$t"

  boot_for "$img" "$t" "$WORK/first-$i.log"

  if ! grep -qa "leanfs" "$WORK/first-$i.log" 2>/dev/null; then
    echo "(cut before the filesystem mounted - no verdict)"
    continue
  fi

  boot_for "$img" 25 "$WORK/second-$i.log"

  problems=""
  if ! out="$(python3 tools/leanfs-fsck.py "$img" 2>&1)"; then
    problems="$out"
  fi

  if grep -qa "KERNEL PANIC" "$WORK/second-$i.log"; then
    problems="${problems}
recovery boot panicked: $(grep -a 'KERNEL PANIC' "$WORK/second-$i.log" | head -1)"
  fi
  if ! grep -qa "PID 1 spawned" "$WORK/second-$i.log"; then
    problems="${problems}
recovery boot never reached the desktop handoff"
  fi

  if [ -z "$problems" ]; then
    pass=$((pass + 1))
    echo "recovered, consistent"
  else
    fail=$((fail + 1))
    echo "FAILED"
    echo "$problems" | sed 's/^/          /'
    cp "$img" "build/crash-failure-$i.bin" 2>/dev/null || true
    cp "$WORK/second-$i.log" "build/crash-failure-$i.log" 2>/dev/null || true
    echo "          image and log kept: build/crash-failure-$i.{bin,log}"
  fi
done

echo
echo "$pass consistent, $fail failed."
[ "$fail" -eq 0 ] || exit 1
