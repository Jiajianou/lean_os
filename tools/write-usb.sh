#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE="build/os-image.bin"

if [ $# -ne 1 ]; then
  echo "Usage: $0 <target-device>" >&2
  echo "Run this with no argument first is not supported on purpose -" >&2
  echo "see the device listing commands below to find the right one." >&2
  echo >&2
  case "$(uname -s)" in
    Darwin) echo "  diskutil list" >&2 ;;
    *)      echo "  lsblk" >&2 ;;
  esac
  exit 1
fi

TARGET="$1"

if [ ! -f "$IMAGE" ]; then
  echo "No image at $IMAGE yet - run 'make' first." >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin)
    if [ "$TARGET" = "/dev/disk0" ]; then
      echo "Refusing to touch /dev/disk0 - that's almost always this Mac's own system disk." >&2
      exit 1
    fi
    if ! diskutil info "$TARGET" >/dev/null 2>&1; then
      echo "diskutil doesn't recognize '$TARGET' as a disk. Run 'diskutil list' and pick a /dev/diskN (the whole disk, not a diskNsM slice)." >&2
      exit 1
    fi
    if [ "$(diskutil info "$TARGET" | awk -F': +' '/Internal/ {print $2}')" = "Yes" ]; then
      echo "'$TARGET' reports itself as an internal (non-removable) disk - refusing." >&2
      echo "This script is only for external USB media." >&2
      exit 1
    fi
    ;;
  Linux)
    if [ "$TARGET" = "/dev/sda" ] || [ "$TARGET" = "/dev/nvme0n1" ]; then
      echo "Refusing to touch '$TARGET' - that's almost always this machine's own system disk." >&2
      exit 1
    fi
    if [ ! -b "$TARGET" ]; then
      echo "'$TARGET' is not a block device. Run 'lsblk' and pick the whole disk (e.g. /dev/sdb, not /dev/sdb1)." >&2
      exit 1
    fi
    REMOVABLE_FLAG="/sys/block/$(basename "$TARGET")/removable"
    if [ -f "$REMOVABLE_FLAG" ] && [ "$(cat "$REMOVABLE_FLAG")" != "1" ]; then
      echo "'$TARGET' doesn't report itself as removable - refusing. This script is only for external USB media." >&2
      exit 1
    fi
    ;;
  *)
    echo "Unsupported platform '$(uname -s)' - write the image manually with dd, carefully." >&2
    exit 1
    ;;
esac

IMAGE_SIZE=$(wc -c < "$IMAGE")
echo "About to overwrite the ENTIRE disk '$TARGET' with $IMAGE ($IMAGE_SIZE bytes)."
echo "Every byte currently on that device will be destroyed. This cannot be undone."
echo
read -r -p "Type the device path again to confirm ($TARGET): " CONFIRM
if [ "$CONFIRM" != "$TARGET" ]; then
  echo "Confirmation did not match - aborting, nothing was written." >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin)
    diskutil unmountDisk "$TARGET"
    RAW_TARGET="${TARGET/\/dev\/disk//dev/rdisk}"
    sudo dd if="$IMAGE" of="$RAW_TARGET" bs=4m status=progress
    diskutil eject "$TARGET"
    ;;
  Linux)
    sudo umount "${TARGET}"?* 2>/dev/null || true
    sudo dd if="$IMAGE" of="$TARGET" bs=4M status=progress conv=fsync
    sudo eject "$TARGET" 2>/dev/null || true
    ;;
esac

echo
echo "Done."
