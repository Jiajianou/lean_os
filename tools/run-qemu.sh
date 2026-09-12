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

# M113: and the browser back onto the image `make all` may have just
# recreated. $(IMAGE)'s recipe rebuilds the disk from scratch whenever
# the kernel changes, which wipes leanfs - so before M113 the browser
# M100 built was gone again after the next kernel edit, and "installed"
# meant "installed until you touch the kernel". This target reinstalls
# it in a tenth of a second and says so, or says the port has not been
# built and carries on: an image without a browser is a valid image, and
# `make browser` is what builds one.
make browser-if-built

# M114: and the screen size, if one was asked for. Same shape and the
# same reason as the line above - the setting lives in /etc/settings.conf
# and `make all` may have just deleted the filesystem it lived in.
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
# M107: two more, and they are the two a machine built in the last twenty
# years actually has. QEMU_DISK=ahci attaches the disk to an ICH9 AHCI
# controller (kernel/drivers/ahci.c) and QEMU_DISK=nvme to an NVMe
# controller (kernel/drivers/nvme.c). Four supported paths now, on
# CLAUDE.md's existing terms: the image is byte-identical across all
# four, and the kernel picks a backend by probing rather than by being
# told - nothing on this command line reaches the guest as a flag.
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
# Q1: the one thing that tells the guest this is a test boot. Absent
# here, so kernel/dev/fwcfg.c's boot_selftests_enabled() reads no such
# file and returns 0. tools/run-tests.sh passes it.
FWCFG_ARGS=()
if [ "$SELFTESTS" -eq 1 ]; then
  FWCFG_ARGS=(-fw_cfg name=opt/leanos/selftest,string=1)
  echo "Booting WITH boot self-tests (expect ~140s to the desktop)." >&2
fi

# ---- M116: a window the size it used to be ------------------------------
#
# QEMU's cocoa display now sizes its window as the guest's pixels divided
# by the Retina backing scale (ui/cocoa.m, resizeWindow) - one guest pixel
# per PHYSICAL pixel. On a Retina Mac that halves the window: the same
# 1024x768 desktop that opened at 1024x768 points opened at 512x384 after
# Homebrew upgraded QEMU to 11.1 on 2026-09-10 (as a side effect of
# installing libpng for the browser build, which is why it looked like
# the browser work had shrunk the screen). Measured with CGWindowList:
# 512x412 points, title bar included.
#
# `zoom-to-fit=on` is not the answer - it opens at 267x228 and waits for
# a drag. What restores the old size is telling macOS this process is not
# high-resolution capable, which makes the window server scale it 2x:
# a bundle whose Info.plist says NSHighResolutionCapable=false, whose
# executable is a symbolic link to the real QEMU. Built under build/ on
# every run, pointing at whatever `qemu-system-x86_64` is on the PATH, so
# a QEMU upgrade needs nothing here. QEMU_HIDPI=1 opts out, for anybody
# who wants one guest pixel per physical pixel. tools/window-test.sh
# measures the window a person gets, because nothing else in the tree
# looks at the host side of the screen at all - which is how this went
# unnoticed through a whole milestone.
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
