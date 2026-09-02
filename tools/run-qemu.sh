#!/usr/bin/env bash
# Builds and boots lean_os in one command (M26: UEFI-only, BIOS boot path
# removed). Always rebuilds the image first so this reflects whatever's
# currently in the working tree, and bootstraps OVMF firmware on first run
# if it isn't there yet - so a completely fresh checkout only needs this
# one script, no separate `make` step.
#
# ---- Q1: this script boots the machine. It does not test it -----------
#
# Until Q1 there was no difference, because the kernel ran every one of
# its ~80 boot self-tests on every boot and there was no way to say
# otherwise. Most of those tests spawn a real process and wait for it, so
# a boot cost ~140 s of which the desktop was about four - and somebody
# who typed `run-qemu.sh` to *use* the machine waited out a test suite
# they had not asked for.
#
# The tests are not gone and are not compiled out; see kernel/dev/fwcfg.h.
# They are off unless the machine is told to run them, and
# tools/run-tests.sh is what tells it. The image is identical either way,
# which is the point: the artifact the harness grades is the artifact that
# boots here.
#
# Usage:
#   tools/run-qemu.sh                # build and boot
#   tools/run-qemu.sh --selftests    # ...with the boot self-tests on
#   tools/run-qemu.sh -- -smp 4      # extra arguments straight to QEMU
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
# host audio device - which is what makes this safe to run headlessly,
# and is enough for everything the driver's self-test asserts (the
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
# M106: stated rather than defaulted, same as tools/qemu-serial-test.sh -
# see the long note there. 1 matches what the battery grades, because a
# desktop that differs from what is tested is worse than a slow one.
# QEMU_CPUS=4 boots the same image on four cores.
QEMU_CPUS=${QEMU_CPUS:-1}
# M92: the disk is a virtio block device rather than the IDE drive a bare
# `-drive` gives on the `pc` machine. kernel/drivers/virtio_blk.c drives
# it; kernel/drivers/ata.c stays as the fallback for anything that has no
# virtio, and QEMU_DISK=ide selects that path so the two can be measured
# against each other. OVMF boots either.
if [ "${QEMU_DISK:-virtio}" = "ide" ]; then
  DISK_ARGS=(-drive "format=raw,file=$IMAGE")
else
  DISK_ARGS=(-drive "if=none,id=disk0,format=raw,file=$IMAGE"
             -device virtio-blk-pci,drive=disk0)
fi
# Q1: the one thing that tells the guest this is a test boot. Absent
# here, so kernel/dev/fwcfg.c's boot_selftests_enabled() reads no such
# file and returns 0. tools/run-tests.sh passes it.
FWCFG_ARGS=()
if [ "$SELFTESTS" -eq 1 ]; then
  FWCFG_ARGS=(-fw_cfg name=opt/leanos/selftest,string=1)
  echo "Booting WITH boot self-tests (expect ~140s to the desktop)." >&2
fi

qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  "${DISK_ARGS[@]}" \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  ${FWCFG_ARGS[@]+"${FWCFG_ARGS[@]}"} \
  ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
