#!/usr/bin/env bash
set -uo pipefail

# Fork out of a process that has other threads running, on a machine with more
# than one core. The graded battery boots ONE core (tools/qemu-serial-test.sh
# defaults QEMU_CPUS to 1), and a core cannot hold a stale translation of its
# own page table write - so the shootdown M165 added is invisible to it. This
# harness exists because a test that cannot fail is not an instrument: with
# smp_tlb_shootdown stubbed out, a single-core boot still passed [m83].
#
# Since M225 it also runs threadtest's region-table check ("regions"): four
# threads mapping, first-touching, splitting and unmapping at once, which is
# the other thing one core cannot be wrong about - a page fault reads the
# region table without its lock while a sibling changes it.
#
# Since M225, threadtest's "leaderless" as well: kill(pid) of a process whose
# main thread has left reaches a thread of it - here, one that may be running
# on another core at that moment - and the process then ends and is reaped.
#
# Since M225, forktest's "descriptors" mode also runs "fdslots": sibling
# threads closing, dup'ing, dup2'ing and forking ONE descriptor at once, which
# released its object twice while the slot itself had no claim on it - one
# core never runs two of them at the same instant. And then "fduse": threads
# blocked in read() and epoll_wait() on descriptors their siblings close,
# which freed the object under the sleeping call.
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
FIRST_ARG="${1:-}"
SECONDS_TO_RUN="${1:-1800}"
# M225: anything after the ceiling goes to QEMU, as with qemu-serial-test.sh:
# QEMU_CPUS=8 tools/fork-smp-test.sh 1800 -accel hvf -cpu host,xlevel=0x80000008
# is the laptop's eight processors running at once, which is where a lost
# reference count update first showed.
shift || true
EXTRA_ARGS=("$@")
QEMU_CPUS=${QEMU_CPUS:-4}
QEMU_MEM=${QEMU_MEM:-4096}

# The verdict, read off the whole log. M225: this stage used to grade lines
# anchored at their start ('^\[forksmp\] exitstorm exited 0'), while each
# [forksmp] line was several kernel_log_* calls that another processor's
# output could land between; 47c3b17 then looked for a line's PIECES in
# order before the next "[forksmp] ", which "[forksmp] exitstorm exited 1"
# followed by any text with a 0 in it satisfied.
#
# M225 (log-crash-path) fixed the root instead: the kernel builds every
# [forksmp] line whole and hands it to the log in ONE kernel_log_write - one
# hold of the ring's lock, so nothing another processor writes can land
# inside it, and the devices are fed in ring order. So every line here is
# graded EXACTLY, newline included, wherever it lands (what is around it is
# another processor's business), and there is no fallback:
#
# - "[forksmp] cores: 0x<8 hex digits>\n";
# - "[forksmp] <mode> exited 0\n" for every mode the kernel runs - a mode
#   that exited anything else has no such line, and fails;
# - the verdict, whole; a panic counts only before it, by position;
# - [logwrite]'s verdict, which runs before [forksmp] in this boot, on the
#   same cores.
#
# LEANOS_FORKSMP_GRADE_LOG=<log> grades a log that already exists, without
# booting anything. `tools/fork-smp-test.sh --check-grader` grades a set of
# made-up logs and requires the right answer for each - one is the
# exitstorm-exited-1 case above, which 47c3b17's grader passed - and every
# boot of this test runs that check first, so the grader is graded before
# what it grades is believed.
FORKSMP_MODES="futex threads threadfork descriptors exitstorm recordlocks regions leaderless"
FORKSMP_VERDICT="[forksmp] fork out of a process with three sibling threads still writing, on every core this machine has - self-test passed."

grade_forksmp_log() {
  FORKSMP_MODES="$FORKSMP_MODES" FORKSMP_VERDICT="$FORKSMP_VERDICT" python3 - "$1" <<'PY'
import os
import re
import sys

path = sys.argv[1]
data = open(path, "rb").read().decode("latin-1").replace("\r", "")

shown = re.compile(r"\[forksmp\]|\[logwrite\] (one write|interrupts off|per character|the console|forktest|the first)")
for line in data.split("\n"):
    if shown.search(line):
        print(line)

def fail(message):
    print("FAIL: " + message)
    sys.exit(1)

LOGWRITE = "[logwrite] one write() is one piece of the log:"
if data.find(LOGWRITE) < 0:
    panic = re.search(r"logwrite self-test:[^\n*]*", data)
    fail("[logwrite] did not pass on this machine - "
         + (panic.group(0).strip() if panic else "its verdict never appeared")
         + " - see " + path)

cores_line = re.search(r"\[forksmp\] cores: 0x([0-9A-F]{8})\n", data)
if not cores_line:
    fail("the machine never reported its core count - see " + path)
cores = int(cores_line.group(1), 16)
if cores < 2:
    fail("this boot had %d core; the whole point is more than one.\n"
         "      Nothing about a shootdown between cores can be decided here." % cores)

# A panic only counts against this test if it happened BEFORE the verdict.
# After it the machine is on its way down, and a four-core power-off that
# faults is a different thing entirely - it has been seen here, garbled
# across several CPUs at once, on a run whose fork checks all passed.
VERDICT = os.environ["FORKSMP_VERDICT"] + "\n"
verdict_at = data.find(VERDICT)
panic_at = data.find("KERNEL PANIC")
if panic_at >= 0 and (verdict_at < 0 or panic_at < verdict_at):
    context = [l for l in data.split("\n")
               if "forktest:" in l or "threadtest:" in l or "KERNEL PANIC" in l]
    for l in context[-5:]:
        print(l)
    fail("fork, the region table or a kill of a process whose main thread left "
         "is wrong on %d cores" % cores)
if verdict_at < 0:
    fail("the self-test never finished - see " + path)

# Every mode by name and exactly, so an image whose kernel or forktest
# predates one cannot pass by not running it, and one that exited anything
# but 0 cannot pass on a 0 somewhere after it.
for mode in os.environ["FORKSMP_MODES"].split():
    if "[forksmp] %s exited 0\n" % mode in data:
        continue
    said = re.findall(r"\[forksmp\] %s exited [^\n]*" % re.escape(mode), data)
    for l in said[-2:]:
        print("  the log has: " + l)
    context = [l for l in data.split("\n") if re.search(r"(forktest|threadtest): " + re.escape(mode), l)]
    for l in context[-3:]:
        print(l)
    fail("%s did not exit 0 on %d cores (no exact '[forksmp] %s exited 0' line) - see %s"
         % (mode, cores, mode, path))
# "descriptors" runs fdslots after its own check - sibling threads closing,
# dup'ing, dup2'ing and forking one descriptor at once, graded by what the
# other end of each object sees, then memfd mappings refused and their
# references given back. Both lines are required by name, so a forktest
# that predates them cannot pass by exiting 0.
# Since fd-use-holds, "descriptors" then runs "fduse": a read, recv or
# epoll_wait blocked on a descriptor its sibling closes goes on with the
# object it started with (Linux's answer), and dup2 onto a number another
# thread is filling says EBUSY. Required by name for the same reason - and
# its "idle named pipe slept" clause too: a threaded process's poll and
# epoll_wait on an idle sys_pipe_open pipe must sleep, graded on the waiting
# thread's processor time (counting each use of a named pipe once made the
# poller's own scan wake it, a core spun to the timeout).
for line in (r"forktest: fdslots - [0-9]+ rounds of", r"forktest: fdslots - 8 memfds of 4 MiB",
             r"forktest: fduse - [0-9]+ rounds of each.*idle named pipe slept"):
    if not re.search(line, data):
        for l in [l for l in data.split("\n")
                  if "forktest: fdslots" in l or "forktest: fduse" in l][-3:]:
            print(l)
        fail("forktest never reported %r on %d cores - see %s" % (line, cores, path))
for l in data.split("\n"):
    if "forktest: fdslots" in l or "forktest: fduse" in l:
        print(l)
print("PASS: fork out of a threaded process is right on %d cores, and one "
      "write() is one piece of the log there." % cores)
PY
}

# Made-up logs, each with the answer the grader must give. A boot's log
# has "\r\n" from the serial port; the grader drops the "\r".
check_grader() {
  local work rc=0 name want got
  work=$(mktemp -d -t forksmp-grader-XXXXXX) || return 1
  write_log() {
    # $1 file; the rest: lines, each written with "\r\n"
    local file="$1"; shift
    : > "$file"
    local l
    for l in "$@"; do printf '%s\r\n' "$l" >> "$file"; done
  }
  local LW="[logwrite] one write() is one piece of the log: 4 programs each wrote 120 lines through write() at the same moment on 4 core(s), and all 480 are in the log whole and once - self-test passed."
  local FD1="forktest: fdslots - 400 rounds of 4 threads closing, dup'ing, dup2'ing and forking one descriptor"
  local FD2="forktest: fdslots - 8 memfds of 4 MiB, each refused an mmap twice and closed: 8 references given back"
  local FD3="forktest: fduse - 40 rounds of each: a read or epoll_wait blocked on a descriptor its sibling closed went on with the object it started with (caught blocked/late: unix 40/0, pipe 40/0, eventfd 40/0, timerfd 40/0, epoll 40/0); dup2 onto a number being filled: 7093 EBUSY in 245892 tries, nothing else; an exec given no environment inherited its process's; idle named pipe slept through poll 0/403 ms and epoll_wait 0/401 ms (processor/wall)"
  local OK_MODES=() m
  for m in $FORKSMP_MODES; do OK_MODES+=("[forksmp] $m exited 0"); done
  local NO_EXITSTORM=() BAD_EXITSTORM=()
  for m in $FORKSMP_MODES; do
    [ "$m" = exitstorm ] && { BAD_EXITSTORM+=("[forksmp] exitstorm exited 1" "[sched] cpu 2 idle 0 ms"); continue; }
    NO_EXITSTORM+=("[forksmp] $m exited 0")
    BAD_EXITSTORM+=("[forksmp] $m exited 0")
  done

  write_log "$work/whole" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${OK_MODES[@]}" "$FORKSMP_VERDICT"
  write_log "$work/exitstorm-1-then-a-0" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${BAD_EXITSTORM[@]}" "$FORKSMP_VERDICT"
  write_log "$work/split-at-a-call" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${NO_EXITSTORM[@]}" "[forksmp] exitstorm[sched] x" " exited 0" "$FORKSMP_VERDICT"
  write_log "$work/no-exitstorm" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${NO_EXITSTORM[@]}" "$FORKSMP_VERDICT"
  write_log "$work/exited-01" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${NO_EXITSTORM[@]}" "[forksmp] exitstorm exited 01" "$FORKSMP_VERDICT"
  write_log "$work/no-logwrite" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${OK_MODES[@]}" "$FORKSMP_VERDICT"
  write_log "$work/logwrite-panic" "*** KERNEL PANIC: logwrite self-test: one write() to the console did not land in the log in one piece ***"
  write_log "$work/one-core" "$LW" "[forksmp] cores: 0x00000001" "$FD1" "$FD2" "$FD3" "${OK_MODES[@]}" "$FORKSMP_VERDICT"
  write_log "$work/cores-split" "$LW" "[forksmp] cores: 0x0000[sched] x" "0004" "$FD1" "$FD2" "$FD3" "${OK_MODES[@]}" "$FORKSMP_VERDICT"
  write_log "$work/panic-before-verdict" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${OK_MODES[@]}" "*** KERNEL PANIC: forksmp: x ***" "$FORKSMP_VERDICT"
  write_log "$work/panic-after-verdict" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${OK_MODES[@]}" "$FORKSMP_VERDICT" "*** KERNEL PANIC: power off ***"
  write_log "$work/verdict-cut" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "$FD3" "${OK_MODES[@]}" "${FORKSMP_VERDICT% - self-test passed.}"
  write_log "$work/no-fdslots" "$LW" "[forksmp] cores: 0x00000004" "${OK_MODES[@]}" "$FORKSMP_VERDICT"
  write_log "$work/no-fduse" "$LW" "[forksmp] cores: 0x00000004" "$FD1" "$FD2" "${OK_MODES[@]}" "$FORKSMP_VERDICT"

  for name in whole:PASS panic-after-verdict:PASS \
              exitstorm-1-then-a-0:FAIL split-at-a-call:FAIL no-exitstorm:FAIL exited-01:FAIL \
              no-logwrite:FAIL logwrite-panic:FAIL one-core:FAIL cores-split:FAIL \
              panic-before-verdict:FAIL verdict-cut:FAIL no-fdslots:FAIL no-fduse:FAIL; do
    want="${name#*:}"
    name="${name%%:*}"
    if grade_forksmp_log "$work/$name" > "$work/$name.out" 2>&1; then got=PASS; else got=FAIL; fi
    if [ "$got" != "$want" ]; then
      echo "fork-smp-test: the grader answered $got for the made-up log '$name', which should $want:"
      sed 's/^/    /' "$work/$name.out"
      rc=1
    fi
  done
  rm -rf "$work"
  [ "$rc" -eq 0 ] && echo "[harness] the grader gives the right answer for 14 made-up logs"
  return "$rc"
}

if [ "$FIRST_ARG" = "--check-grader" ]; then
  check_grader
  exit $?
fi

if [ -n "${LEANOS_FORKSMP_GRADE_LOG:-}" ]; then
  [ -f "$LEANOS_FORKSMP_GRADE_LOG" ] || { echo "no log at $LEANOS_FORKSMP_GRADE_LOG" >&2; exit 1; }
  grade_forksmp_log "$LEANOS_FORKSMP_GRADE_LOG"
  exit $?
fi

check_grader || exit 1

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
    -serial file:"$LOG" -monitor none ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} &
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

  if grep -aqF "[forksmp] cores:" "$LOG"; then
    break
  fi
  # [logwrite] runs before [forksmp] and is graded here too, so a boot that
  # failed it DID reach a test, and its verdict is as final as fork's.
  # ("logwrite self-test:" is only ever a panic's message, one call, so it is
  # found whole even if another processor's output split the panic's line.)
  if grep -aqF "logwrite self-test:" "$LOG"; then
    break
  fi
  if [ "$attempt" -ge 3 ]; then
    echo "FAIL: three boots and the machine never reached the test at all."
    echo "      That is the four-core boot rather than fork - see $LOG"
    exit 1
  fi
  echo "[harness] the boot never reached the test; trying again"
done

grade_forksmp_log "$LOG"
exit $?
