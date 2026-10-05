#!/usr/bin/env bash
# tools/nic-stream-test.sh - [m116]'s stream from the host, on the accelerator
# this host has, at the memory sizes a laptop has.
#
#   tools/nic-stream-test.sh [MiB ...]     default: 4096 16384
#
# Boots the image once per size with opt/leanos/nicstreamonly, which runs
# [m102] (every frame touched and given back) and then the stream, first
# thing, and powers the machine off; and holds nic_stream_recv_ms to its row
# in tests/budgets.tsv. About a minute for both sizes under hvf.
#
# Why this exists beside the battery, which runs the same stream: the battery
# runs it once, at 4 GiB, under TCG, minutes in. Under hvf the stream cost
# 17.4 s against a 1000 ms ceiling, all of it QEMU on macOS stopping the
# whole guest while SLIRP forked the stream's `cat` (tools/nic-stream-env.sh
# has the measurements), and the freeze grew with the RAM the guest had
# touched - about 4.3 s per GiB, so 68 s at the ThinkPad's 16 GiB. Nothing
# TCG ran could see it, and hvf is the accelerator that behaves like the
# laptop's silicon. The [m102] step is what makes this an instrument: without
# it the guest has touched little, and the unfixed harness measured 412 ms
# and 439 ms here - inside the ceiling. With LEANOS_PRIVATE_GUEST_RAM=1 (no
# shared RAM) this fails at both sizes on an Intel Mac, 17632 ms and
# 68209 ms when it was written; shared, 76 ms and 80 ms. That is the A/B.
#
# The accelerator follows the host the way tools/memory-size-test.sh's does:
# `-accel hvf -cpu host,xlevel=0x80000008` on an x86_64 Mac with Hypervisor.framework, TCG
# anywhere else (an arm64 Mac's hvf cannot run an x86 guest). QEMU_ACCEL=
# tcg|hvf overrides it; QEMU_CPUS defaults to 1, like every other harness.
set -uo pipefail

cd "$(dirname "$0")/.."

SIZES=("$@")
if [ ${#SIZES[@]} -eq 0 ]; then
  SIZES=(4096 16384)
fi
CEILING_S=${NIC_STREAM_TEST_CEILING:-900}
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
QEMU_CPUS=${QEMU_CPUS:-1}
BUDGET_NAME=nic_stream_recv_ms

if [ ! -f "$IMAGE" ]; then
  echo "nic-stream-test: no image at $IMAGE - run 'make' first" >&2
  exit 1
fi
if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
  echo "nic-stream-test: no OVMF at build/ovmf - run tools/build-ovmf.sh" >&2
  exit 1
fi
CEILING_MS=$(awk -F'\t' -v n="$BUDGET_NAME" '$1 == n {print $2; exit}' tests/budgets.tsv)
if [ -z "$CEILING_MS" ]; then
  echo "nic-stream-test: tests/budgets.tsv has no $BUDGET_NAME row" >&2
  exit 1
fi

# hw.optional.arm64 as well as uname -m: a shell under Rosetta on Apple
# Silicon says x86_64 to uname, and its hvf still cannot run an x86 guest.
ACCEL="${QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
  ACCEL=tcg
  if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "x86_64" ] &&
     [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" != "1" ] &&
     [ "$(sysctl -n kern.hv_support 2>/dev/null)" = "1" ]; then
    ACCEL=hvf
  fi
fi
case "$ACCEL" in
  hvf) ACCEL_ARGS=(-accel hvf -cpu host,xlevel=0x80000008) ;;
  tcg) ACCEL_ARGS=(-accel tcg) ;;
  *) echo "nic-stream-test: QEMU_ACCEL must be hvf or tcg, not $ACCEL" >&2; exit 2 ;;
esac

. tools/nic-stream-env.sh

mkdir -p build
FAILED=0
QEMU_PID=""
VARS=""
NIC_STREAM=""
cleanup() {
  if [ -n "$QEMU_PID" ]; then
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
  fi
  [ -n "$VARS" ] && rm -f "$VARS"
  [ -n "$NIC_STREAM" ] && rm -f "$NIC_STREAM"
}
trap cleanup EXIT

# The machine fills its memory before the stream ([m102], see kernel.c's
# nicstreamonly), so a guest of MiB costs the host MiB of real memory for the
# length of the boot. A size over half of this host's RAM is not asked for -
# an 8 GB Mac paging a 16 GiB guest out would be measuring its own disk - and
# says so, rather than passing quietly.
HOST_MIB=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1048576 ))
RAN=0

nic_stream_prepare
for MIB in "${SIZES[@]}"; do
  if [ "$HOST_MIB" -gt 0 ] && [ $(( MIB * 2 )) -gt "$HOST_MIB" ]; then
    echo "nic-stream-test: SKIP $MIB MiB - this host has $HOST_MIB MiB, and a guest that fills its memory needs at most half of that"
    continue
  fi
  RAN=$(( RAN + 1 ))
  LOG="build/nic-stream-$MIB.log"
  : > "$LOG"
  VARS=$(mktemp -t leanos-nicstream-vars-XXXXXX)
  cp "$OVMF_VARS_TEMPLATE" "$VARS"
  guest_ram_args "$MIB"
  echo "nic-stream-test: $MIB MiB, $QEMU_CPUS cpu(s), $ACCEL, ${GUEST_RAM_ARGS[*]}"
  qemu-system-x86_64 "${ACCEL_ARGS[@]}" "${GUEST_RAM_ARGS[@]}" -smp "$QEMU_CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
    -device virtio-blk-pci,drive=disk0 -display none \
    -netdev "$NETDEV" -device rtl8139,netdev=net0 \
    -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
    -device qemu-xhci,id=xhci0 -device usb-kbd,bus=xhci0.0 -device usb-mouse,bus=xhci0.0 \
    -fw_cfg name=opt/leanos/selftest,string=1 \
    -fw_cfg "name=opt/leanos/nicstream,string=$NIC_STREAM_BYTES" \
    -fw_cfg name=opt/leanos/nicstreamonly,string=1 \
    -serial file:"$LOG" -monitor none 2> "$LOG.qemu" &
  QEMU_PID=$!

  deadline=$(( $(date +%s) + CEILING_S ))
  while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -qF "[nicstream] done" "$LOG" 2>/dev/null ||
       grep -qF "*** KERNEL PANIC:" "$LOG" 2>/dev/null ||
       ! kill -0 "$QEMU_PID" 2>/dev/null; then
      break
    fi
    sleep 1
  done
  kill "$QEMU_PID" 2>/dev/null || true
  wait "$QEMU_PID" 2>/dev/null || true
  QEMU_PID=""
  rm -f "$VARS"
  VARS=""

  grep -aE '^netrecv:' "$LOG" | sed 's/^/  /'
  if grep -qF "*** KERNEL PANIC:" "$LOG"; then
    echo "  FAIL: $(grep -aF '*** KERNEL PANIC:' "$LOG" | head -1) - see $LOG"
    FAILED=1
    continue
  fi
  if ! grep -qF "[m116] a stream from the host:" "$LOG"; then
    echo "  FAIL: the stream never finished within ${CEILING_S}s - see $LOG"
    tail -5 "$LOG" "$LOG.qemu" | sed 's/^/    /'
    FAILED=1
    continue
  fi
  MS=$(grep -aoE "^\[perf\] $BUDGET_NAME [0-9]+ ms" "$LOG" | tail -1 | awk '{print $3}')
  if [ -z "$MS" ]; then
    echo "  FAIL: no [perf] $BUDGET_NAME line - see $LOG"
    FAILED=1
  elif [ "$MS" -gt "$CEILING_MS" ]; then
    echo "  FAIL: $BUDGET_NAME $MS ms at $MIB MiB under $ACCEL - over its ${CEILING_MS} ms ceiling"
    FAILED=1
  else
    echo "  ok: $BUDGET_NAME $MS ms at $MIB MiB under $ACCEL (ceiling ${CEILING_MS})"
  fi
done

if [ "$FAILED" -ne 0 ]; then
  echo "nic-stream-test: FAIL"
  exit 1
fi
if [ "$RAN" -eq 0 ]; then
  echo "nic-stream-test: FAIL - every size was skipped, so nothing was measured"
  exit 1
fi
echo "nic-stream-test: pass ($RAN size(s) measured)"
