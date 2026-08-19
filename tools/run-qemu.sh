#!/usr/bin/env bash
# Boots the OS image in QEMU. Filled in once M1/M2 produce a bootable image.
set -euo pipefail

IMAGE="build/os-image.bin"

if [ ! -f "$IMAGE" ]; then
  echo "No image at $IMAGE yet — nothing to boot (see milestones.md, M1)." >&2
  exit 1
fi

qemu-system-x86_64 -drive format=raw,file="$IMAGE"
