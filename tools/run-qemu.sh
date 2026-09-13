#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

SELFTESTS=0
EXTRA_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --selftests) SELFTESTS=1; shift ;;
    --) shift; EXTRA_ARGS=("$@"); break ;;
    *) echo "unknown argument: $1 (see this script's header)" >&2; exit 2 ;;
  esac
done

make all

make browser-if-built

if [ -n "${QEMU_RES:-}" ]; then
  ./tools/set-resolution.sh "$QEMU_RES" || exit 1
fi

IMAGE="build/os-image.bin"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
OVMF_VARS_RUNTIME="build/ovmf/OVMF_VARS.runtime.fd"

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
  echo "No OVMF firmware at build/ovmf/ yet - building it now from source" >&2
  echo "(tools/build-ovmf.sh; one-time, several minutes)..." >&2
  ./tools/build-ovmf.sh
fi

cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS_RUNTIME"

QEMU_MEM=${QEMU_MEM:-4096}
QEMU_CPUS=${QEMU_CPUS:-1}
case "${QEMU_DISK:-virtio}" in
  ide)
    DISK_ARGS=(-drive "format=raw,file=$IMAGE")
    ;;
  ahci)
    DISK_ARGS=(-device ich9-ahci,id=ahci0
               -drive "if=none,id=disk0,format=raw,file=$IMAGE"
               -device ide-hd,drive=disk0,bus=ahci0.0)
    ;;
  nvme)
    DISK_ARGS=(-drive "if=none,id=disk0,format=raw,file=$IMAGE"
               -device nvme,drive=disk0,serial=leanos0)
    ;;
  *)
    DISK_ARGS=(-drive "if=none,id=disk0,format=raw,file=$IMAGE"
               -device virtio-blk-pci,drive=disk0)
    ;;
esac
FWCFG_ARGS=()
if [ "$SELFTESTS" -eq 1 ]; then
  FWCFG_ARGS=(-fw_cfg name=opt/leanos/selftest,string=1)
  echo "Booting WITH boot self-tests (expect ~140s to the desktop)." >&2
fi

QEMU_BIN=$(command -v qemu-system-x86_64)
if [ "$(uname)" = "Darwin" ] && [ "${QEMU_HIDPI:-0}" != "1" ] && [ -n "$QEMU_BIN" ]; then
  APP="build/qemu-app/lean_os.app"
  mkdir -p "$APP/Contents/MacOS"
  cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>qemu-system-x86_64</string>
  <key>CFBundleIdentifier</key><string>org.leanos.run-qemu</string>
  <key>CFBundleName</key><string>lean_os</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>NSHighResolutionCapable</key><false/>
</dict></plist>
PLIST
  ln -sfn "$QEMU_BIN" "$APP/Contents/MacOS/qemu-system-x86_64"
  QEMU_BIN="$APP/Contents/MacOS/qemu-system-x86_64"
fi

"$QEMU_BIN" \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  "${DISK_ARGS[@]}" \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  ${FWCFG_ARGS[@]+"${FWCFG_ARGS[@]}"} \
  ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
