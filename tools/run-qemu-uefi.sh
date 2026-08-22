#!/usr/bin/env bash
# Boots the same build/os-image.bin as tools/run-qemu.sh, but through
# UEFI firmware (OVMF) instead of BIOS - the M24 alternate boot path.
# Needs tools/build-ovmf.sh run at least once first (builds build/ovmf/
# from source; see that script's header comment for why).
set -euo pipefail

IMAGE="build/os-image.bin"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
OVMF_VARS_RUNTIME="build/ovmf/OVMF_VARS.runtime.fd"

if [ ! -f "$IMAGE" ]; then
  echo "No image at $IMAGE yet — run 'make' first." >&2
  exit 1
fi

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
  echo "No OVMF firmware at build/ovmf/ yet — run 'tools/build-ovmf.sh' first." >&2
  exit 1
fi

# OVMF_VARS.fd is firmware NVRAM (boot order, etc.) that the running VM
# writes back to - copied per-run so a boot doesn't mutate the shared
# template every other invocation depends on staying pristine.
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS_RUNTIME"

qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive format=raw,file="$IMAGE"
