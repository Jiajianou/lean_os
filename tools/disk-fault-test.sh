#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)
IMAGE="$ROOT/build/os-image.bin"
OVMF_CODE="$ROOT/build/ovmf/OVMF_CODE.fd"
OVMF_VARS="$ROOT/build/ovmf/OVMF_VARS.fd"

[ -f "$IMAGE" ] || { echo "disk-fault-test: no image - run make first" >&2; exit 1; }
[ -f "$OVMF_CODE" ] || { echo "disk-fault-test: no OVMF - run tools/build-ovmf.sh" >&2; exit 1; }

SECONDS_TO_RUN="${LEANOS_FAULT_SECONDS:-120}"

WORK=$(mktemp -d -t leanos-blkdebug-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

run_one() {
  local once=$1 label=$2
  cp "$OVMF_VARS" "$WORK/vars.fd"
  local log="$WORK/serial-$once.log"
  : > "$log"
  cat > "$WORK/blkdebug.conf" <<CONF
[inject-error]
event = "write_aio"
errno = "5"
once = "$once"
CONF

  echo "disk-fault-test: $label, through ${QEMU_DISK:-virtio}"

  local backing="blkdebug:$WORK/blkdebug.conf:$IMAGE"
  local disk_args
  if [ "${QEMU_DISK:-virtio}" = "ide" ]; then
    disk_args=(-drive "format=raw,snapshot=on,file=$backing")
  else
    disk_args=(-drive "if=none,id=disk0,format=raw,snapshot=on,file=$backing"
               -device virtio-blk-pci,drive=disk0)
  fi

  qemu-system-x86_64 \
    -m "${QEMU_MEM:-512}" -smp 1 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$WORK/vars.fd" \
    "${disk_args[@]}" -display none \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
    -serial file:"$log" -monitor none 2>/dev/null &
  local pid=$!
  disown "$pid" 2>/dev/null || true

  local i
  for i in $(seq 1 "$SECONDS_TO_RUN"); do
    if grep -q "\[init\] PID 1 spawned" "$log" 2>/dev/null; then break; fi
    if grep -q "KERNEL PANIC" "$log" 2>/dev/null; then break; fi
    sleep 1
  done
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true

  if grep -q "KERNEL PANIC" "$log"; then
    echo "FAIL: $label panicked the machine:" >&2
    grep -n "KERNEL PANIC" "$log" >&2
    cp "$log" "$ROOT/build/disk-fault-serial.log" 2>/dev/null || true
    return 1
  fi
  if ! grep -q "\[init\] PID 1 spawned" "$log"; then
    echo "FAIL: $label left a machine that never reached PID 1." >&2
    tail -20 "$log" >&2
    cp "$log" "$ROOT/build/disk-fault-serial.log" 2>/dev/null || true
    return 1
  fi
  echo "  ok - booted to PID 1"
  return 0
}

rc=0
run_one on  "one refused write" || rc=1
run_one off "every write refused, for the whole boot" || rc=1
if [ "$rc" -ne 0 ]; then
  exit 1
fi

echo "PASS: a disk that refuses writes under ${QEMU_DISK:-virtio} leaves a machine that boots."
