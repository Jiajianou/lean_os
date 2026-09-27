#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE="build/os-image.bin"
OUTPUT="build/os-image-hardware.bin"
VIDEO="native"
INTERRUPTS=""
CPUS=""
LOG_SECTORS=49152

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
  --log-mib <n>                     size of \LOGS\LEANOS.LOG, the file the
                                    kernel writes its log into; default 24,
                                    0 for none. tools/read-usb-log.sh reads it.
  --output <path>                   default: build/os-image-hardware.bin
USAGE
  exit 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --video) VIDEO="$2"; shift 2 ;;
    --interrupts) INTERRUPTS="$2"; shift 2 ;;
    --cpus) CPUS="$2"; shift 2 ;;
    --log-mib) LOG_SECTORS=$(( $2 * 2048 )); shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done

if [ ! -f "$IMAGE" ]; then
  echo "No image at $IMAGE yet - run 'make all' first." >&2
  exit 1
fi

ESP_START_LBA=$(make -s print-esp-start-lba)
ESP_SECTORS=$(make -s print-esp-sector-count)
ESP="$OUTPUT@@$(( ESP_START_LBA * 512 ))"

CONFIG="$(mktemp -t lean_os_cfg)"
LOG_FILE="$(mktemp -t lean_os_log)"
trap 'rm -f "$CONFIG" "$LOG_FILE"' EXIT

echo "Copying $IMAGE -> $OUTPUT"
cp "$IMAGE" "$OUTPUT"

# The kernel has no FAT driver and does not need one: the log file is
# allocated here, on a freshly formatted partition, and the kernel is told
# which sectors it occupies. Those sectors are found by looking for the
# header rather than by working out cluster arithmetic, and then the whole
# file is compared with those sectors - which is what proves it is contiguous,
# since a file in two pieces would not read back as one run of the disk.
LOG_OPTION=""
if [ "$LOG_SECTORS" -gt 0 ]; then
  python3 tools/read-usb-log.py --make-area "$LOG_SECTORS" > "$LOG_FILE"
  mmd -i "$ESP" ::LOGS 2>/dev/null || true
  mcopy -i "$ESP" -o "$LOG_FILE" ::LOGS/LEANOS.LOG
  LOG_LBA=$(python3 tools/read-usb-log.py --locate "$OUTPUT" "$ESP_START_LBA" "$ESP_SECTORS" "$LOG_FILE")
  LOG_OPTION="log=$LOG_LBA+$LOG_SECTORS"
fi
{
  echo "# lean_os boot options - read by \\EFI\\BOOT\\BOOTX64.EFI before it picks"
  echo "# a graphics mode. This file is a property of the machine rather than of"
  echo "# the image, which is why it lives here and not in the kernel."
  echo "video=$VIDEO"
  [ -n "$INTERRUPTS" ] && echo "interrupts=$INTERRUPTS"
  [ -n "$CPUS" ] && echo "cpus=$CPUS"
  if [ -n "$LOG_OPTION" ]; then
    echo "# \\LOGS\\LEANOS.LOG, as sectors of this disk. Found by the image tool;"
    echo "# the kernel refuses to write there unless the file's header is there."
    echo "$LOG_OPTION"
  fi
  true
} > "$CONFIG"

mcopy -i "$ESP" -o "$CONFIG" ::EFI/BOOT/lean_os.cfg

echo "Wrote \\EFI\\BOOT\\lean_os.cfg:"
sed 's/^/    /' "$CONFIG"
echo
echo "$OUTPUT is $(wc -c < "$OUTPUT") bytes."
echo "Write it to the whole disk of the machine it is for - not to a partition."
