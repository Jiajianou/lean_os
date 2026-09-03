#!/usr/bin/env bash
# tools/disk-fault-test.sh - Q16: a disk that fails, from below the driver.
#
# The [q16] boot self-test injects its faults at kernel/drivers/blk.c,
# which is the right place for almost everything: it is where the two
# block drivers meet, so one test grades whichever backend the machine
# has. What it cannot grade is the drivers themselves. A fault injected
# above `ata_read_sectors` never reaches the ERR bit that
# ata_wait_ready() polls for, and never reaches virtio's status byte -
# and those are exactly the six halts Q16 converted.
#
# QEMU's `blkdebug` driver injects below both of them. It fails a real
# request at a real event, so the ERR status and the non-zero virtio
# status byte are produced by the emulator rather than by this project,
# and the code that has to notice them is the code that ships.
#
# ---- what this asserts ------------------------------------------------
#
# That the machine BOOTS THROUGH IT. Not that it reports a particular
# error - it may not even need the sector that fails, depending where the
# failure lands - but that a disk which refuses a request does not stop
# the machine. Before Q16 every one of these was a panic, so this script
# would have produced a kernel panic on the serial line and nothing else.
#
# The bar is deliberately the low one, because it is the one that
# changed, and because a script that demanded a specific error message
# would be asserting which request happens to be Nth on a given boot -
# a number that moves whenever anything in the boot order does.
#
# ---- why a WRITE and not a read ---------------------------------------
#
# The first thing tried here was a failing read, and it failed the wrong
# thing: the firmware loads the kernel off this disk before any of this
# project's code runs, so a read error lands in `BdsDxe` and the serial
# log says "lean_os uefi: ReadBlocks failed loading the kernel". That is
# a true statement about a disk whose boot sectors are unreadable and it
# grades nothing this milestone changed.
#
# A write cannot happen before the kernel is up, because nothing writes
# to a disk before it has mounted one. So the first write after boot is
# both guaranteed to be this kernel's and guaranteed to reach the driver
# - which is exactly the request Q16 turned from a halt into a report.
# Recorded here rather than fixed silently, because "inject the fault
# where only your own code can hit it" is the general version and it is
# the part that took a run to learn.
#
# Usage:
#   tools/disk-fault-test.sh            # virtio, an error on the first write
#   QEMU_DISK=ide tools/disk-fault-test.sh
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

# Two runs, and the second is the milestone's own bar.
#
#   once=on   a single failed request. The narrow case: does one bad
#             sector stop the machine.
#   once=off  EVERY write fails, for the whole boot. Q16's "How we'll
#             know" asks for a disk that fails every write from the tenth
#             onward, and this is that with the tenth brought forward -
#             a machine that boots to PID 1 through it has taken the
#             failure path hundreds of times rather than once, which is
#             also what makes this test hard to pass vacuously.
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
