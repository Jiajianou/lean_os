#!/usr/bin/env bash
# Builds and boots lean_os in one command (M26: UEFI-only, BIOS boot path
# removed). Always rebuilds the image first so this reflects whatever's
# currently in the working tree, and bootstraps OVMF firmware on first run
# if it isn't there yet - so a completely fresh checkout only needs this
# one script, no separate `make` step.
set -euo pipefail

cd "$(dirname "$0")/.."

make all

IMAGE="build/os-image.bin"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
OVMF_VARS_RUNTIME="build/ovmf/OVMF_VARS.runtime.fd"

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
  echo "No OVMF firmware at build/ovmf/ yet - building it now from source" >&2
  echo "(tools/build-ovmf.sh; one-time, several minutes)..." >&2
  ./tools/build-ovmf.sh
fi

# OVMF_VARS.fd is firmware NVRAM (boot order, etc.) that the running VM
# writes back to - copied per-run so a boot doesn't mutate the shared
# template every other invocation depends on staying pristine.
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS_RUNTIME"

qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive format=raw,file="$IMAGE"
