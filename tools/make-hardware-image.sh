#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE="build/os-image.bin"
OUTPUT="build/os-image-hardware.bin"
VIDEO="native"
INTERRUPTS=""
CPUS=""

usage() {
  cat >&2 <<'USAGE'
Usage: tools/make-hardware-image.sh [options]

Writes a copy of build/os-image.bin with \EFI\BOOT\lean_os.cfg in its EFI
system partition - the file the boot loader reads before it picks a graphics
mode. The QEMU image is left alone, because every coordinate in the input
suite is measured against its 1024x768 framebuffer.

  --video native|<width>x<height>   default: native (the panel's own mode)
  --interrupts pic|ioapic           default: unset, which means the I/O APIC
                                    on a machine with no QEMU fw_cfg device
  --cpus <n>                        default: unset, which means every one
  --output <path>                   default: build/os-image-hardware.bin
USAGE
  exit 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --video) VIDEO="$2"; shift 2 ;;
    --interrupts) INTERRUPTS="$2"; shift 2 ;;
    --cpus) CPUS="$2"; shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done

if [ ! -f "$IMAGE" ]; then
  echo "No image at $IMAGE yet - run 'make all' first." >&2
  exit 1
fi

ESP_START_LBA=$(make -s print-esp-start-lba)

CONFIG="$(mktemp -t lean_os_cfg)"
trap 'rm -f "$CONFIG"' EXIT
{
  echo "# lean_os boot options - read by \\EFI\\BOOT\\BOOTX64.EFI before it picks"
  echo "# a graphics mode. This file is a property of the machine rather than of"
  echo "# the image, which is why it lives here and not in the kernel."
  echo "video=$VIDEO"
  [ -n "$INTERRUPTS" ] && echo "interrupts=$INTERRUPTS"
  [ -n "$CPUS" ] && echo "cpus=$CPUS"
} > "$CONFIG"

echo "Copying $IMAGE -> $OUTPUT"
cp "$IMAGE" "$OUTPUT"
mcopy -i "$OUTPUT"@@$(( ESP_START_LBA * 512 )) -o "$CONFIG" ::EFI/BOOT/lean_os.cfg

echo "Wrote \\EFI\\BOOT\\lean_os.cfg:"
sed 's/^/    /' "$CONFIG"
echo
echo "$OUTPUT is $(wc -c < "$OUTPUT") bytes."
echo "Write it to the whole disk of the machine it is for - not to a partition."
