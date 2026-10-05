#!/usr/bin/env bash
# tools/memory-size-test.sh - the machine says how much memory it has, and
# it is the memory QEMU was given.
#
#   tools/memory-size-test.sh [MiB ...]     default: 4096 16384
#
# Boots the image once per size (-m MiB) with the self-tests off, reads the
# "[inventory] memory: N MiB of RAM" line every boot prints, and requires
# N to be at most what -m said and no more than SLACK_MIB below it.
#
# Why this exists: until it, the kernel counted memory as the highest usable
# address divided by a page - the SPAN its frame bitmap has to cover - and a
# 16 GiB guest printed "17408 MiB usable", because QEMU puts 3 GiB below the
# PCI hole at 3-4 GiB and the other 13 GiB above it. The same number reached
# sysinfo(2), sysconf(_SC_PHYS_PAGES), Settings' About page and the image
# cache's budget, and the real 16 GB laptop of M188 was written down as an
# "18 GB" machine from that print. The span is still what the frame
# allocator tracks; it is no longer what the machine REPORTS.
#
# SLACK_MIB is what the firmware may keep for itself inside the RAM it was
# given - OVMF's runtime code and data, ACPI tables and NVS, the legacy hole
# at 640K-1M - none of which the boot loader hands over as usable. Measured
# when this was written (QEMU 11.1.2, this tree's OVMF, hvf on an Intel Mac):
# 128 -> 122, 4096 -> 4090, 16384 -> 16378 - 6 MiB at every size. 64 is ten
# times that, room for a firmware rebuild rather than a tolerance tuned to
# pass, and still far tighter than anything it is meant to catch: a count
# that lost the memory above 4 GiB, or below it, is GiBs short. The
# misreport this was written for is a whole GiB ABOVE -m, so the upper bound
# - never more than QEMU was told - takes no tolerance at all.
#
# The accelerator follows the host: on an x86_64 Mac with Hypervisor.framework
# it is `-accel hvf -cpu host,xlevel=0x80000008`; anywhere else (arm64, where hvf cannot run an
# x86 guest) it is TCG. The line comes seconds into a boot either way - both
# sizes took 6 s together under TCG on an Intel Mac. QEMU_ACCEL=tcg|hvf
# overrides the choice. QEMU_CPUS defaults to 1, like every other harness.
set -uo pipefail

cd "$(dirname "$0")/.."

SIZES=("$@")
if [ ${#SIZES[@]} -eq 0 ]; then
  SIZES=(4096 16384)
fi
SLACK_MIB=64
CEILING_S=${MEMORY_TEST_CEILING:-240}
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
QEMU_CPUS=${QEMU_CPUS:-1}

if [ ! -f "$IMAGE" ]; then
  echo "memory-size-test: no image at $IMAGE - run 'make' first" >&2
  exit 1
fi
if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
  echo "memory-size-test: no OVMF at build/ovmf - run tools/build-ovmf.sh" >&2
  exit 1
fi

# hw.optional.arm64 is asked as well as uname -m because a shell running
# under Rosetta on Apple Silicon says x86_64 to uname, and its hvf still
# cannot run an x86 guest.
ACCEL="${QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
  ACCEL=tcg
  if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "x86_64" ] &&
     [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" != "1" ] &&
     [ "$(sysctl -n kern.hv_support 2>/dev/null)" = "1" ]; then
    ACCEL=hvf
  fi
fi
# xlevel because QEMU's bare `-cpu host` under hvf reports a highest
# extended CPUID leaf below 0x80000004, which hides the brand string the
# laptop's own CPUID has.
case "$ACCEL" in
  hvf) ACCEL_ARGS=(-accel hvf -cpu host,xlevel=0x80000008) ;;
  tcg) ACCEL_ARGS=(-accel tcg) ;;
  *) echo "memory-size-test: QEMU_ACCEL must be hvf or tcg, not $ACCEL" >&2; exit 2 ;;
esac

mkdir -p build
FAILED=0
QEMU_PID=""
VARS=""
cleanup() {
  if [ -n "$QEMU_PID" ]; then
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
  fi
  [ -n "$VARS" ] && rm -f "$VARS"
}
trap cleanup EXIT

for MIB in "${SIZES[@]}"; do
  LOG="build/memory-size-$MIB.log"
  : > "$LOG"
  VARS=$(mktemp -t leanos-memory-vars-XXXXXX)
  cp "$OVMF_VARS_TEMPLATE" "$VARS"
  echo "memory-size-test: -m $MIB, $QEMU_CPUS cpu(s), $ACCEL"
  qemu-system-x86_64 "${ACCEL_ARGS[@]}" -m "$MIB" -smp "$QEMU_CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
    -device virtio-blk-pci,drive=disk0 \
    -display none -serial file:"$LOG" -monitor none 2> "$LOG.qemu" &
  QEMU_PID=$!

  deadline=$(( $(date +%s) + CEILING_S ))
  LINE=""
  while [ "$(date +%s)" -lt "$deadline" ]; do
    LINE=$(grep -m1 '^\[inventory\] memory:' "$LOG" 2>/dev/null || true)
    [ -n "$LINE" ] && break
    if grep -qF "*** KERNEL PANIC:" "$LOG" 2>/dev/null; then
      break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
      break
    fi
    sleep 1
  done
  kill "$QEMU_PID" 2>/dev/null || true
  wait "$QEMU_PID" 2>/dev/null || true
  QEMU_PID=""
  rm -f "$VARS"
  VARS=""

  grep -m1 '^\[pmm\]' "$LOG" | sed 's/^/  /'
  if [ -z "$LINE" ]; then
    echo "  FAIL: no [inventory] memory line within ${CEILING_S}s - see $LOG"
    tail -5 "$LOG" "$LOG.qemu" | sed 's/^/    /'
    FAILED=1
    continue
  fi
  echo "  $LINE"
  RAM=$(echo "$LINE" | sed -n 's/^\[inventory\] memory: \([0-9][0-9]*\) MiB.*/\1/p')
  if [ -z "$RAM" ]; then
    echo "  FAIL: the line does not begin with a number of MiB"
    FAILED=1
    continue
  fi
  if [ "$RAM" -gt "$MIB" ]; then
    echo "  FAIL: $RAM MiB reported on a machine given $MIB - more memory than exists"
    FAILED=1
  elif [ "$RAM" -lt $(( MIB - SLACK_MIB )) ]; then
    echo "  FAIL: $RAM MiB reported on a machine given $MIB - more than ${SLACK_MIB} MiB missing"
    FAILED=1
  else
    echo "  ok: $RAM MiB of the $MIB QEMU was given ($(( MIB - RAM )) MiB kept by the firmware)"
  fi
  # The same boot's processor line, because hvf's bare `-cpu host` hides the
  # brand leaves (see ACCEL_ARGS) and the inventory then names no processor.
  CPU_LINE=$(grep -m1 '^\[inventory\] processor:' "$LOG" 2>/dev/null || true)
  if [ -z "$CPU_LINE" ] || echo "$CPU_LINE" | grep -qF '(no brand string)'; then
    echo "  FAIL: the processor has no brand string under $ACCEL: ${CPU_LINE:-no [inventory] processor line}"
    FAILED=1
  else
    echo "  ok: ${CPU_LINE#\[inventory\] processor: }"
  fi
done

if [ "$FAILED" -ne 0 ]; then
  echo "memory-size-test: FAIL"
  exit 1
fi
echo "memory-size-test: pass"
