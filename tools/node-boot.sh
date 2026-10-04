#!/usr/bin/env bash
set -uo pipefail

# M223. Boot the machine, run one script through /bin/node, power off.
#
# The battery reaches Node minutes in; this reaches it in seconds, because
# opt/leanos/node makes the kernel run the script before any self-test and
# then switch the machine off. With a local file as the argument, that file
# is put on the image first, under /lib/node-test.

cd "$(dirname "$0")/.."

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_NODE_LOG:-build/node-serial.log}"
SCRIPT="${1:-/lib/node-test/smoke.js}"
SECONDS_TO_RUN="${2:-600}"

if [ -f "$SCRIPT" ]; then
  make -s leanfs-put > /dev/null 2>&1
  build/leanfs-put "$IMAGE" "$SCRIPT" "/lib/node-test/$(basename "$SCRIPT")" > /dev/null || exit 1
  SCRIPT="/lib/node-test/$(basename "$SCRIPT")"
fi

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
[ -f "$OVMF_CODE" ] || ./tools/build-ovmf.sh || exit 1
OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_node.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

echo "[harness] node $SCRIPT - ceiling ${SECONDS_TO_RUN}s"
qemu-system-x86_64 \
  -m "${QEMU_MEM:-4096}" -smp "${QEMU_CPUS:-1}" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
  -device virtio-blk-pci,drive=disk0 -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  -fw_cfg "name=opt/leanos/node,string=$SCRIPT${LEANOS_NODE_ARGS:+ $LEANOS_NODE_ARGS}" \
  -serial file:"$LOG" -monitor none &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
while kill -0 "$QEMU_PID" 2>/dev/null; do
  grep -q "\[node\] done\|KERNEL PANIC" "$LOG" 2>/dev/null && break
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "node-boot: the machine did not finish within ${SECONDS_TO_RUN}s" >&2
    break
  fi
  sleep 2
done
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

sed -n '/\[node\] running/,$p' "$LOG"
grep -q "\[node\] exit 0" "$LOG"
