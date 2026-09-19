#!/usr/bin/env bash
set -uo pipefail

# Fork out of a process that has other threads running, on a machine with more
# than one core. The graded battery boots ONE core (tools/qemu-serial-test.sh
# defaults QEMU_CPUS to 1), and a core cannot hold a stale translation of its
# own page table write - so the shootdown M165 added is invisible to it. This
# harness exists because a test that cannot fail is not an instrument: with
# smp_tlb_shootdown stubbed out, a single-core boot still passed [m83].
#
# It boots the ordinary battery - which is the configuration four cores are
# known to survive - and adds opt/leanos/forksmp, which runs the fork modes
# early and powers the machine off. Early, because a four-core boot of the
# whole battery trips over the [m55] flake long before it reaches [m83]; and
# inside the battery rather than instead of it, because a boot with the
# self-test switch off stops in xHCI enumeration on this host.

cd "$(dirname "$0")/.."

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_FORKSMP_LOG:-build/forksmp-serial.log}"
SECONDS_TO_RUN="${1:-600}"
QEMU_CPUS=${QEMU_CPUS:-4}
QEMU_MEM=${QEMU_MEM:-4096}

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
if [ ! -f "$OVMF_CODE" ]; then
  ./tools/build-ovmf.sh || exit 1
fi
[ -f "$IMAGE" ] || { echo "fork-smp-test: no image at $IMAGE - run make first" >&2; exit 1; }

OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_forksmp.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

echo "[harness] fork out of a threaded process - ceiling ${SECONDS_TO_RUN}s, ${QEMU_CPUS} cpu(s)"

qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
  -device virtio-blk-pci,drive=disk0 -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  -device qemu-xhci,id=xhci0 \
  -device usb-kbd,bus=xhci0.0 \
  -device usb-mouse,bus=xhci0.0 \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  -fw_cfg name=opt/leanos/forksmp,string=1 \
  -serial file:"$LOG" -monitor none &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
while kill -0 "$QEMU_PID" 2>/dev/null; do
  if grep -q "\[forksmp\] fork out of a process\|KERNEL PANIC" "$LOG" 2>/dev/null; then
    break
  fi
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "fork-smp-test: the machine did not finish within ${SECONDS_TO_RUN}s" >&2
    break
  fi
  sleep 2
done
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

cores=$(grep -o "\[forksmp\] cores: 0x[0-9A-Fa-f]*" "$LOG" | tail -1 | sed 's/.*0x//')
if [ -z "$cores" ]; then
  echo "FAIL: the machine never reported its core count - see $LOG"
  exit 1
fi
if [ $(( 0x$cores )) -lt 2 ]; then
  echo "FAIL: this boot had $(( 0x$cores )) core; the whole point is more than one."
  echo "      Nothing about a shootdown between cores can be decided here."
  exit 1
fi

grep -E "^\[forksmp\]" "$LOG"
if grep -q "KERNEL PANIC" "$LOG"; then
  grep -E "forktest:" "$LOG" | tail -5
  echo "FAIL: fork out of a process with sibling threads is wrong on $(( 0x$cores )) cores"
  exit 1
fi
if ! grep -q "\[forksmp\] fork out of a process with three sibling" "$LOG"; then
  echo "FAIL: the self-test never finished - see $LOG"
  exit 1
fi
echo "PASS: fork out of a threaded process is right on $(( 0x$cores )) cores."
