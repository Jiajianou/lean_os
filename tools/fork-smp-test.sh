#!/usr/bin/env bash
set -uo pipefail

# Fork out of a process that has other threads running, on a machine with more
# than one core. The graded battery boots ONE core (tools/qemu-serial-test.sh
# defaults QEMU_CPUS to 1), and a core cannot hold a stale translation of its
# own page table write - so the shootdown M165 added is invisible to it. This
# harness exists because a test that cannot fail is not an instrument: with
# smp_tlb_shootdown stubbed out, a single-core boot still passed [m83].
#
# It boots the ordinary battery - which is the configuration four cores are
# known to survive - and adds opt/leanos/forksmp, which runs the fork modes
# early and powers the machine off. Early, because a four-core boot of the
# whole battery trips over the [m55] flake long before it reaches [m83]; and
# inside the battery rather than instead of it, because a boot with the
# self-test switch off stops in xHCI enumeration on this host.

cd "$(dirname "$0")/.."

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_FORKSMP_LOG:-build/forksmp-serial.log}"
# The ceiling is here to stop a hang, not to measure how fast a boot is. A
# four-core boot on this machine is slow and gets much slower when the host
# is busy - measured at 95 s healthy and past 600 s with nothing wrong,
# reaching twelve markers and no panic. An interleaved A/B of six runs each,
# alternating kernels so host drift lands on both, put the failure rate at
# 5/6 on either side: the flake is the configuration, not the change. So the
# ceiling is generous and what this stage actually grades is the [forksmp]
# lines below.
SECONDS_TO_RUN="${1:-1800}"
QEMU_CPUS=${QEMU_CPUS:-4}
QEMU_MEM=${QEMU_MEM:-4096}

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
if [ ! -f "$OVMF_CODE" ]; then
  ./tools/build-ovmf.sh || exit 1
fi
[ -f "$IMAGE" ] || { echo "fork-smp-test: no image at $IMAGE - run make first" >&2; exit 1; }

OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_forksmp.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

# Up to three attempts, and only for boots that never reached the test.
#
# A four-core boot of this machine is the configuration CLAUDE.md's notes
# already call flaky, and it fails in two ways that have nothing to do with
# fork: it stalls (twelve markers in 600 s, no panic), and it panics in the
# scheduler on the IDLE task's kernel stack. An interleaved A/B - six runs
# each, alternating this kernel with the one before it so host load lands on
# both - put the rate at 5/6 on either side. So a boot that dies before
# [forksmp] runs says nothing about fork, and retrying it is not papering
# over a failure: the moment the block DOES run, its verdict is final and
# there is no retry.
attempt=0
while : ; do
  attempt=$(( attempt + 1 ))
  : > "$LOG"
  echo "[harness] fork out of a threaded process - attempt $attempt, ceiling ${SECONDS_TO_RUN}s, ${QEMU_CPUS} cpu(s)"

  qemu-system-x86_64 \
    -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
    -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
    -device virtio-blk-pci,drive=disk0 -display none \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
    -device qemu-xhci,id=xhci0 \
    -device usb-kbd,bus=xhci0.0 \
    -device usb-mouse,bus=xhci0.0 \
    -fw_cfg name=opt/leanos/selftest,string=1 \
    -fw_cfg name=opt/leanos/forksmp,string=1 \
    -serial file:"$LOG" -monitor none &
  QEMU_PID=$!
  disown "$QEMU_PID" 2>/dev/null || true

  deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
  while kill -0 "$QEMU_PID" 2>/dev/null; do
    if grep -q "\[forksmp\] fork out of a process\|KERNEL PANIC" "$LOG" 2>/dev/null; then
      break
    fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
      echo "fork-smp-test: the machine did not finish within ${SECONDS_TO_RUN}s" >&2
      break
    fi
    sleep 2
  done
  kill "$QEMU_PID" 2>/dev/null
  wait "$QEMU_PID" 2>/dev/null

  if grep -q "\[forksmp\] cores:" "$LOG"; then
    break
  fi
  if [ "$attempt" -ge 3 ]; then
    echo "FAIL: three boots and the machine never reached the test at all."
    echo "      That is the four-core boot rather than fork - see $LOG"
    exit 1
  fi
  echo "[harness] the boot never reached the test; trying again"
done

cores=$(grep -o "\[forksmp\] cores: 0x[0-9A-Fa-f]*" "$LOG" | tail -1 | sed 's/.*0x//')
if [ -z "$cores" ]; then
  echo "FAIL: the machine never reported its core count - see $LOG"
  exit 1
fi
if [ $(( 0x$cores )) -lt 2 ]; then
  echo "FAIL: this boot had $(( 0x$cores )) core; the whole point is more than one."
  echo "      Nothing about a shootdown between cores can be decided here."
  exit 1
fi

grep -E "^\[forksmp\]" "$LOG"

# A panic only counts against this test if it happened BEFORE the verdict.
# After it the machine is on its way down, and a four-core power-off that
# faults is a different thing entirely - it has been seen here, garbled
# across several CPUs at once, on a run whose fork checks all passed.
verdict_line=$(grep -n "\[forksmp\] fork out of a process with three sibling" "$LOG" |
               head -1 | cut -d: -f1)
panic_line=$(grep -n "KERNEL PANIC" "$LOG" | head -1 | cut -d: -f1)
if [ -n "$panic_line" ] && { [ -z "$verdict_line" ] || [ "$panic_line" -lt "$verdict_line" ]; }; then
  grep -E "forktest:" "$LOG" | tail -5
  echo "FAIL: fork out of a process with sibling threads is wrong on $(( 0x$cores )) cores"
  exit 1
fi
if ! grep -q "\[forksmp\] fork out of a process with three sibling" "$LOG"; then
  echo "FAIL: the self-test never finished - see $LOG"
  exit 1
fi
echo "PASS: fork out of a threaded process is right on $(( 0x$cores )) cores."
