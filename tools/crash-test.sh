#!/usr/bin/env bash
# tools/crash-test.sh - Q17: cut the power, and see what the filesystem
# looks like afterwards.
#
# ---- Why this exists --------------------------------------------------
#
# M71 is titled "Files worth trusting". Its mechanism is write ordering -
# the data block reaches the disk before the metadata that points at it -
# plus a mount check that notices a filesystem that was not unmounted
# cleanly. Both are good, and neither had ever been tested by an unclean
# mount, because nothing in this project had ever cut power to the
# machine mid-write. The one guarantee the filesystem makes was the one
# nothing checked.
#
# So: boot, kill QEMU with SIGKILL at a chosen instant - not a shutdown,
# not a reboot, the power going off - then boot the same image again and
# check it with tools/leanfs-fsck.py, which is a deliberately independent
# reader for the reason its own header gives.
#
# The window this aims at is the first boot, and that is not laziness. A
# first boot formats the disk and then seeds ~50 programs into /bin,
# which is the heaviest run of metadata writes this machine ever does -
# every one of them an inode allocation, a bitmap update and a directory
# insert. If write ordering is going to fail anywhere it is there, and it
# needs no workload program to arrange.
#
# Usage:
#   tools/crash-test.sh              # 12 crashes across the write window
#   tools/crash-test.sh 30           # more of them
#   tools/crash-test.sh 12 4.0 18.0  # ...between these two instants
set -uo pipefail

cd "$(dirname "$0")/.."

# Measured rather than guessed. OVMF takes about eight seconds before
# the kernel starts, so a cut before then proves nothing and the script
# says so rather than counting it. The filesystem mounts at ~8.5s and the
# heavy metadata run - format, then seeding ~50 programs into /bin -
# lasts until ~15s. That window is what this aims at.
RUNS="${1:-16}"
MIN_T="${2:-8.5}"
MAX_T="${3:-15.5}"

IMAGE="build/os-image.bin"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS="build/ovmf/OVMF_VARS.fd"

for f in "$IMAGE" "$OVMF_CODE" "$OVMF_VARS"; do
  [ -f "$f" ] || { echo "missing $f - run 'make' first" >&2; exit 1; }
done

WORK="$(mktemp -d -t leanos-crash-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# One QEMU, one image, killed after `secs`. `mode` picks whether the kill
# is a power cut or a clean exit at the end of the run.
boot_for() {
  local img="$1" secs="$2" log="$3"
  local vars="$WORK/vars.fd"
  cp "$OVMF_VARS" "$vars"
  qemu-system-x86_64 -m 4096 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$vars" \
    -drive if=none,id=d0,format=raw,file="$img" -device virtio-blk-pci,drive=d0 \
    -display none -netdev user,id=n0 -device rtl8139,netdev=n0 \
    -audiodev none,id=s0 -device AC97,audiodev=s0 \
    -serial file:"$log" -monitor none &
  local pid=$!
  # Fractional sleeps: the interesting instants are hundreds of
  # milliseconds apart, not seconds.
  perl -e "select undef, undef, undef, $secs" 2>/dev/null || sleep "${secs%.*}"
  # SIGKILL, deliberately. SIGTERM would let QEMU flush, which is the
  # opposite of the event being simulated.
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

pass=0
fail=0
echo "Q17: $RUNS power cuts between ${MIN_T}s and ${MAX_T}s into a first boot."
echo

for i in $(seq 1 "$RUNS"); do
  # Spread the kill instants evenly across the window rather than
  # randomly: an even sweep covers the format and the seeding loop
  # predictably, and a run that finds nothing is then a statement about
  # the whole window rather than about wherever the dice landed.
  t=$(awk -v i="$i" -v n="$RUNS" -v a="$MIN_T" -v b="$MAX_T" \
          'BEGIN { printf "%.2f", a + (b - a) * (i - 1) / (n > 1 ? n - 1 : 1) }')

  img="$WORK/crash-$i.bin"
  cp "$IMAGE" "$img"

  printf "  %2d/%2d  cut at %5ss  " "$i" "$RUNS" "$t"

  boot_for "$img" "$t" "$WORK/first-$i.log"

  # Did the guest get far enough to have written anything? A cut before
  # the filesystem is even mounted proves nothing and should not be
  # counted as a pass.
  if ! grep -qa "leanfs" "$WORK/first-$i.log" 2>/dev/null; then
    echo "(cut before the filesystem mounted - no verdict)"
    continue
  fi

  # Now the part that matters: the machine comes back. This exercises
  # the unclean-mount path for real rather than by writing DIRTY into a
  # superblock by hand.
  boot_for "$img" 25 "$WORK/second-$i.log"

  problems=""
  if ! out="$(python3 tools/leanfs-fsck.py "$img" 2>&1)"; then
    problems="$out"
  fi

  # A panic on the recovery boot is a failure even if the image is
  # structurally fine: a filesystem you cannot mount is not a filesystem
  # that survived.
  if grep -qa "KERNEL PANIC" "$WORK/second-$i.log"; then
    problems="${problems}
recovery boot panicked: $(grep -a 'KERNEL PANIC' "$WORK/second-$i.log" | head -1)"
  fi
  if ! grep -qa "PID 1 spawned" "$WORK/second-$i.log"; then
    problems="${problems}
recovery boot never reached the desktop handoff"
  fi

  if [ -z "$problems" ]; then
    pass=$((pass + 1))
    echo "recovered, consistent"
  else
    fail=$((fail + 1))
    echo "FAILED"
    echo "$problems" | sed 's/^/          /'
    cp "$img" "build/crash-failure-$i.bin" 2>/dev/null || true
    cp "$WORK/second-$i.log" "build/crash-failure-$i.log" 2>/dev/null || true
    echo "          image and log kept: build/crash-failure-$i.{bin,log}"
  fi
done

echo
echo "$pass consistent, $fail failed."
[ "$fail" -eq 0 ] || exit 1
