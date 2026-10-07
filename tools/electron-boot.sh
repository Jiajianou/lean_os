#!/usr/bin/env bash
set -uo pipefail

# M227. Boot the machine, run one Electron app, power off - node-boot.sh's
# loop for the program VS Code is written for. The battery's [m227] captures
# Electron's output into a buffer and keeps only the end of it; this streams
# all of it to the serial line as it is written, so a hang shows how far it
# got and a crash shows its backtrace.
#
#   tools/electron-boot.sh [app-directory] [seconds] [extra qemu arguments...]
#
# With a local directory as the first argument, it is put on the image as
# /lib/electron-test/<its name> first. LEANOS_ELECTRON_ARGS adds switches
# before the app (--enable-logging=stderr is always there).

cd "$(dirname "$0")/.."

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_ELECTRON_LOG:-build/electron-serial.log}"
APP="${1:-/lib/electron-test/app}"
SECONDS_TO_RUN="${2:-600}"
shift 2 2>/dev/null || shift $#

if [ -d "$APP" ]; then
  make -s leanfs-put > /dev/null 2>&1
  build/leanfs-put -r "$IMAGE" "$APP" "/lib/electron-test/$(basename "$APP")" > /dev/null || exit 1
  APP="/lib/electron-test/$(basename "$APP")"
fi

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
[ -f "$OVMF_CODE" ] || ./tools/build-ovmf.sh || exit 1
OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_electron.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

COMMAND="/usr/lib/electron/electron --enable-logging=stderr ${LEANOS_ELECTRON_ARGS:+$LEANOS_ELECTRON_ARGS }$APP"
echo "[harness] $COMMAND - ceiling ${SECONDS_TO_RUN}s"
qemu-system-x86_64 \
  -m "${QEMU_MEM:-4096}" -smp "${QEMU_CPUS:-1}" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
  -device virtio-blk-pci,drive=disk0 -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  -fw_cfg "name=opt/leanos/run,string=$COMMAND" \
  -serial file:"$LOG" -monitor none "$@" &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
while kill -0 "$QEMU_PID" 2>/dev/null; do
  grep -q "\[run\] done\|KERNEL PANIC" "$LOG" 2>/dev/null && break
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "electron-boot: the machine did not finish within ${SECONDS_TO_RUN}s" >&2
    break
  fi
  sleep 2
done
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

sed -n '/\[run\] running/,$p' "$LOG"
grep -q "\[run\] exit 0" "$LOG"
