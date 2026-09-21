#!/usr/bin/env bash
set -uo pipefail

# Chromium's own browser, asked to draw a page.
#
# The graded battery reaches the browser 470 seconds into a boot, and what is
# being iterated on here is one program: so this boots the machine with
# opt/leanos/browser=1, which runs /bin/chromiumshell on one page and powers
# off. Nothing else in the battery runs.

cd "$(dirname "$0")/.."

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_BROWSER_LOG:-build/browser-serial.log}"
SECONDS_TO_RUN="${1:-900}"
QEMU_CPUS=${QEMU_CPUS:-1}
QEMU_MEM=${QEMU_MEM:-4096}

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
[ -f "$OVMF_CODE" ] || ./tools/build-ovmf.sh || exit 1
[ -f "$IMAGE" ] || { echo "browser-test: no image at $IMAGE - run make first" >&2; exit 1; }

OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_browser.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

echo "[harness] Chromium's own browser drawing a page - ceiling ${SECONDS_TO_RUN}s"
qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
  -device virtio-blk-pci,drive=disk0 -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  -fw_cfg name=opt/leanos/browser,string=1 \
  -serial file:"$LOG" -monitor none &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
while kill -0 "$QEMU_PID" 2>/dev/null; do
  if grep -q "\[browser\] done\|KERNEL PANIC" "$LOG" 2>/dev/null; then
    break
  fi
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "browser-test: the machine did not finish within ${SECONDS_TO_RUN}s" >&2
    break
  fi
  sleep 5
done
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

if grep -q "\[m169\] /bin/chromiumshell is not on this image" "$LOG"; then
  grep -E "^\[m169\]" "$LOG" | tail -2
  echo "SKIP: this image has no browser on it."
  exit 0
fi

grep -E "^\[m169\]" "$LOG" | tail -3

if grep -q "KERNEL PANIC" "$LOG"; then
  grep -E "^\[pmm\]|KERNEL PANIC" "$LOG" | tail -5
  echo "FAIL: the machine panicked while the browser was running - see $LOG"
  exit 1
fi
if ! grep -q "^\[browser\] done" "$LOG"; then
  echo "FAIL: the browser run never finished - see $LOG"
  exit 1
fi

# The picture, graded here rather than on the machine: a PNG decoder belongs
# on the side that can be told it is wrong by something else.
python3 tools/browser-shot.py "$LOG" --save build/browser-shot.png
rc=$?
if [ $rc -ne 0 ]; then
  echo "      the decoded picture is in build/browser-shot.png"
  exit 1
fi
echo "      the picture is in build/browser-shot.png"
