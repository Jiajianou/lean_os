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

# -netdev user is QEMU's built-in SLIRP NAT - no root/tap setup needed.
# It always hands the guest 10.0.2.15 and answers as gateway 10.0.2.2
# itself, the two addresses kernel/net/net.h hardcodes (no DHCP client -
# see its header comment).
#
# M62: -device AC97 is the sound card kernel/drivers/ac97.c drives. The
# `none` audiodev means QEMU emulates the controller without opening a
# host audio device - which is what makes this safe to run headlessly and
# in CI, and is enough for everything the driver's self-test asserts (the
# device consumed the buffer it was handed). Swap it for `coreaudio`,
# `pa` or `sdl` to actually hear the thing.
# M90: the machine's memory size, stated rather than defaulted. Every
# boot before M90 ran on whatever `qemu-system-x86_64` picks when nobody
# says - 128 MiB - and no file in this tree recorded that, which made "how
# much memory does lean_os have" a question with no answer in the
# repository. 4 GiB is chosen for a specific reason and not for headroom:
# QEMU splits it across the PCI hole, so the guest gets a RAM region above
# the 4 GiB mark and the kernel's frame allocator has to handle a physical
# address that does not fit in 32 bits. A round 2 GiB would have been
# entirely below the hole and would have tested nothing that 1 GiB did
# not.
QEMU_MEM=${QEMU_MEM:-4096}
qemu-system-x86_64 \
  -m "$QEMU_MEM" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive format=raw,file="$IMAGE" \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0
