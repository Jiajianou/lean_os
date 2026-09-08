#!/usr/bin/env bash
# Boots build/os-image.bin headlessly via OVMF (M26: UEFI-only, BIOS boot
# path removed), captures COM1 (see kernel/drivers/serial.c) to a file for
# SECONDS, then grades the capture as a real pass/fail regression check -
# every klog_* message (drivers/klog.h) reaches this capture, including
# every boot-time self-test's own "X self-test passed" line.
#
# M29: this used to just dump the log and exit 0 unconditionally, leaving
# "did anything actually break" a read-the-log-and-eyeball-it job for a
# human - fine when this was the only milestone or two deep, not once
# growing UX/app complexity means a regression in, say, M14's pipe
# self-test could scroll by unnoticed under a screenful of M22 GUI output.
# Now it fails loudly (nonzero exit, a summary of exactly what's missing)
# if the kernel panicked or any REQUIRED_MARKERS entry never showed up -
# meant to run before every milestone from here on, not just when
# something looks wrong.
#
# M40: `snapshot=on` on the disk. Guest writes (leanfs formats the disk on
# its first boot) go to a throwaway overlay instead of back into
# build/os-image.bin, which fixes two things at once: this script no longer
# takes a write lock on the image - so it can run alongside
# tools/qemu-input-test.sh or a `make` - and every run is genuinely the
# from-scratch, unformatted-disk boot SECONDS_TO_RUN's budget below is
# written against. Before this, only the first run after a rebuild was;
# every one after that booted the already-formatted disk the previous run
# left behind, quietly skipping the format path it claims to cover.
#
# M40: this remains the *boot-time* check only. Everything a human has to
# click to reach now has its own harness - tools/qemu-input-test.sh, which
# injects real mouse/keyboard events and grades real framebuffer pixels.
# Run both before every milestone; neither subsumes the other (see that
# script's header for what this one structurally cannot see).
#
# Usage: tools/qemu-serial-test.sh [SECONDS] [-- extra qemu args, e.g. -monitor pipe:/tmp/mon for key injection]
set -euo pipefail

# Enough to carry a from-scratch (unformatted-disk) boot through every
# self-test below and into the desktop handoff (the last REQUIRED_MARKERS
# entry) - see the M29 progress notes for the timings the original 24s was
# measured against. A real budget, not a magic number to leave stale: bump
# it when a milestone adds enough boot-time work to push past it, which
# M40 (two new self-tests) and M41 (a fourth desktop client, and every
# self-test's own compositor now opening four more pipes) between them
# did - 32s was measured failing and 34s passing, so 40 leaves real
# headroom rather than sitting one slow boot away from a false failure.
#
# M42/M43 both added a self-test and the boot got *faster*: 30s now passes
# comfortably. M42's selftest_reap is why - every self-test now waits for
# the clients it kills instead of leaving them to be noticed some
# arbitrary number of scheduler quanta later, and a killed compositor that
# keeps getting scheduled is not free. The budget stayed at 40 through M44;
# the headroom is the point.
#
# M45: 40 -> 50. The new self-test spawns two compositor clients and waits
# out a deliberate "did the polite close *fail*?" interval, which cannot be
# shortened - proving something did not happen takes real time. 44s was
# measured passing, so 50 keeps the same "not one slow boot from a false
# failure" margin the number has always been chosen for.
#
# M46: 50 -> 56, for one more self-test that spawns a compositor and two
# clients.
#
# M47: 56 -> 64. Its self-test starts two whole desktops (a compositor and
# a desktop_icons each) to read a real painted pixel back, plus a second
# ~1s grace period it has to wait out to prove nothing needed SIGKILL.
#
# M51-M56: 96 -> 300, in two steps (180, then 300 once a boot was
# measured needing more than that). Six new self-tests, and three of them are the slow
# kind for the same unavoidable reason: proving something about a
# *process* means starting one and waiting for it. M52 spawns a program
# that deliberately faults 1.2s in and then waits for the compositor to
# notice; M54 runs 384 spawn/reap rounds; M55 starts two whole
# compositors to kill one out from under the other's clients; M56 types
# into a real editor and a real terminal and waits for each to answer.
# 96 was measured passing at the end of M54 with less than one
# self-test's margin, which is exactly the "one slow boot from a false
# failure" case this number has always been chosen to avoid.
#
# The honest caveat: boot time is now *variable*, not just larger - 65s
# on a quiet host and past 180s on a busy one, because most of the added
# time is self-tests waiting on real processes and those wait on the
# scheduler rather than on a fixed clock. This is a ceiling, not an
# estimate; as of Q1 the run really does stop as soon as the last marker
# appears, which is what the rest of this sentence claimed for several
# milestones before anything implemented it. Five milestones, five new self-tests, and three of
# them are the slow kind for the same unavoidable reason: proving
# something about a *process* means starting one and waiting for it. M52
# spawns a program that deliberately faults 1.2s in and then waits for
# the compositor to notice; M54 runs 384 spawn/reap rounds; M55 starts
# two whole compositors and four clients to kill one of them out from
# under the others. The previous 96 was measured passing at the end of
# M54 and failing during M55 - by less than one self-test, which is
# exactly the "one slow boot from a false failure" margin this number has
# always been chosen to avoid.
#
# M48: 64 -> 72. Proving a toast is gone *by its own deadline* means
# waiting out that deadline (TOAST_TTL_MS, 4s) and then some - there is no
# shorter way to check that something stopped being on screen on its own.
#
# M58: 300 -> 360. Its desktop self-test lets a resolution change's
# revert countdown (WM_MODE_REVERT_MS, 10s) expire without confirming it,
# because that path only ever runs when something has already gone wrong
# and is therefore the one most worth a test. Same unavoidable shape as
# M48's toast deadline: proving something happened *on its own* means
# waiting for it.
# M69: 820 -> 240, and this is the first time this number has been
# *measured* rather than bumped.
#
# It has only ever gone up - 24, 34, 40, 50, 56, ... 740, 820 - each time
# a milestone added a self-test and the capture ran out before the last
# marker. Nobody ever asked how long the boot actually took, because
# nothing said. It says now ("[boot] reached the desktop handoff in N s")
# and the answer was **140 seconds against an 820-second budget**: every
# run spent eleven minutes waiting for a machine that had finished.
#
# Two things made it 140. Most of the fixed pit_sleep_ms calls that
# preceded a compositor startup are now selftest_wait_for_compositor,
# which returns when the desktop is painted instead of after a guessed
# interval. And the boot was never as slow as the budget implied - the
# budget was slack piled on slack.
#
# 240 is 70% headroom over a measured 140, which is the same
# not-one-slow-boot-from-a-false-failure margin this number has always
# been chosen for - it is just measured against something real now. The
# boot prints its own time, so the next person to see this fail knows
# immediately whether the machine got slower or the budget got tight.
#
# M75-M77: 240 -> 400. Three new self-tests, and two of them are the slow
# kind for the reason every slow one here is slow: they spawn a real
# program and wait for it. M75 runs two children and a shell script, M76
# waits for a process to reach a ready point it announces by creating a
# file (a fixed sleep would be a race in whichever direction the machine
# was slow that boot) and then waits out a second process's death, and
# M77 spawns a tree walker. 240 was measured *failing* mid-M76 at exactly
# the point the boot printed its last line, which is the "one slow boot
# from a false failure" case this number is always chosen to avoid.
#
# M100 (fourth and fifth increments): 400 -> 600. The battery measured
# 360 s with freetype and expat in it - expat's own 4,392 checks are
# 50 s of that, the single most expensive self-test on the machine and
# budgeted as such in tests/budgets.tsv - and the first boot with sqlite
# added hit 400 exactly, with thirty markers still to come. 600 is the
# same not-one-slow-boot margin over a ~400 s battery; the boot prints
# boot_to_desktop_s, so the next failure here says which side moved.
SECONDS_TO_RUN="${1:-600}"
shift || true
EXTRA_ARGS=("$@")

# Overridable for the same reason tools/qemu_input.py's IMAGE is: a
# `make` in this tree rewrites build/os-image.bin under a guest that is
# still reading it. Snapshot the image and export LEANOS_IMAGE to run a
# long check against a frozen one.
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
OVMF_VARS_RUNTIME="$(mktemp -t qemu-serial-ovmf-vars-XXXXXX.fd)"
# M103: LEANOS_IOAPIC=1 routes every legacy line through the I/O APIC
# instead of the 8259. Off by default because it costs roughly a factor
# of two under QEMU and nothing at all on real hardware - the numbers are
# in kernel/dev/fwcfg.h, next to the switch itself. `--full` turns it on
# for a second pass, which is what makes "the whole battery green with
# the PIC masked" a thing this project has actually run.
IOAPIC_FWCFG=""
if [ "${LEANOS_IOAPIC:-0}" = "1" ]; then
  IOAPIC_FWCFG="-fw_cfg name=opt/leanos/ioapic,string=1"
fi

LOG="${LEANOS_SERIAL_LOG:-$(mktemp -t qemu-serial-XXXXXX.log)}"
# Emptied before QEMU is started, and this is not tidiness. The wait loop
# below decides the boot is over by grepping this file for a panic or for
# the desktop handoff - so a named log left over from a previous run
# makes the harness see the PREVIOUS boot's ending one second in, kill
# QEMU, and report every marker as missing. Cost: half an hour, twice.
: > "$LOG"

if [ ! -f "$IMAGE" ]; then
  echo "No image at $IMAGE yet - run 'make' first." >&2
  exit 1
fi

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_TEMPLATE" ]; then
  echo "No OVMF firmware at build/ovmf/ yet - building it now from source" >&2
  echo "(tools/build-ovmf.sh; one-time, several minutes)..." >&2
  ./tools/build-ovmf.sh
fi

cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS_RUNTIME"

# -netdev user: see tools/run-qemu.sh's comment - needed here too since
# the boot self-test (kernel/kernel.c) pings the gateway and would panic
# without a NIC attached at all.
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
# M106: the number of cores, stated rather than defaulted - and the reason
# is the same one M90 gave for stating the memory size, with a much worse
# bill attached.
#
# Every graded boot in this project's history ran on ONE core, because
# nothing here ever passed -smp and QEMU's default is 1. This kernel has
# had SMP since M7: an AP trampoline, per-CPU GDTs and TSSes, a
# scheduler-tick IPI, per-CPU current-task and slice state, and a [smp]
# self-test that printed "1 CPU(s) online" every single time and passed.
# The self-test was true and it was measuring nothing.
#
# What that hid, found within a day of setting this to 4: the AP
# trampoline had triple-faulted the machine since M91, `smp_current_cpu`
# aliased an unrecognised core onto cpu 0, a reaped task's kernel stack
# was freed while it was still standing on it, a spawned task inherited a
# dead task's scheduling class, and M101's profiler sampled the boot CPU
# alone. See M106's notes for all five.
#
# The default is 1 and not 4, and that is a statement about where this
# stands rather than a preference: the battery is green on one core and
# is NOT yet green on more - the remaining failures are named in
# milestones.md. tools/smp-test.sh is the multi-core stage that IS green,
# and it grades the part this milestone finished.
QEMU_CPUS=${QEMU_CPUS:-1}
# M92: virtio-blk rather than the default IDE drive - see tools/run-qemu.sh.
# QEMU_DISK=ide runs the same image through kernel/drivers/ata.c instead,
# which is how the two numbers in the [m92] line get compared.
if [ "${QEMU_DISK:-virtio}" = "ide" ]; then
  DISK_ARGS=(-drive "format=raw,snapshot=on,file=$IMAGE")
else
  DISK_ARGS=(-drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE"
             -device virtio-blk-pci,drive=disk0)
fi
qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  "${DISK_ARGS[@]}" -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  $IOAPIC_FWCFG \
  -serial file:"$LOG" -monitor none ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

# Q1: wait for the boot to *finish*, not for the budget to expire.
#
# This was `sleep "$SECONDS_TO_RUN"` - unconditionally, for the whole
# budget - while the comment block above claimed "the run stops as soon as
# the last marker appears". It did not. The boot has printed its own
# duration since M69 and the answer was ~140 s against a 400 s budget, so
# every run of this script donated a little over four minutes to a machine
# that had finished. That sentence was the only untrue one in this file
# and this loop is what makes it true.
#
# SECONDS_TO_RUN is now what its name always implied - a ceiling - and the
# two ways out are the two real outcomes: the boot reached its last marker,
# or it panicked. Nothing is graded here; a boot that ends either way still
# goes through the full marker check below, so an early exit can never turn
# a failure into a pass.
FINAL_MARKER="[init] PID 1 spawned"
deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
outcome="timeout"
while [ "$(date +%s)" -lt "$deadline" ]; do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    outcome="qemu exited"
    break
  fi
  if grep -qF "$FINAL_MARKER" "$LOG" 2>/dev/null; then
    outcome="reached the desktop handoff"
    # The handoff line is not quite the end of the log: [m68]'s idle-tick
    # measurement and anything else that runs on the way out still has to
    # land. A second is enough for lines already in flight and is not a
    # guess about work that has not started - every REQUIRED_MARKERS entry
    # is printed before this one.
    sleep 1
    break
  fi
  if grep -qF "*** KERNEL PANIC:" "$LOG" 2>/dev/null; then
    outcome="kernel panic"
    sleep 1
    break
  fi
  sleep 1
done
elapsed=$(( $(date +%s) - (deadline - SECONDS_TO_RUN) ))

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

cat "$LOG"

echo
echo "[harness] $outcome after ${elapsed}s (ceiling ${SECONDS_TO_RUN}s)."

# M29: every boot-time self-test's own "passed"/"verified" klog line -
# kept as literal substrings of kernel/kernel.c's own klog_puts calls
# (not regexes) so a wording tweak there is a visible one-line diff here
# too, rather than a silently-still-passing check that stopped meaning
# what its neighbors say it means. Order matches boot order. The last
# entry (init handoff) isn't itself a self-test - it's the "boot actually
# reached steady state" checkpoint everything above is a prerequisite for.
REQUIRED_MARKERS=(
  "[vmm] map/unmap self-test passed."
  "[heap] kmalloc/kfree self-test passed."
  "[fb] framebuffer clear/fill/readback self-test passed."
  "[m58] display mode set and read back from the device (geometry,"
  "[font39] glyph table + shared-baseline render self-test passed."
  "[sched] back on the main task - preemption round trip verified."
  "[syscall] SYS_exit self-test task ran and terminated."
  "[pipe] kernel-level producer/consumer self-test passed."
  "[pipe] SYS_pipe/SYS_write/SYS_read self-test passed."
  "[signal] SIGTERM self-test passed"
  "[wait] SYS_wait(-1) self-test passed"
  "[pgid] SYS_getpgid self-test passed"
  "[fs] leanfs indirect-block self-test passed"
  "[memtest] user-space malloc/free and cross-process shm self-tests passed."
  "[m57] proportional UI font: per-glyph advances, one shared baseline across"
  "[wm] compositor + client self-test passed"
  "[wm21] multi-window compositor + focus-routing self-test passed"
  "[wm22] desktop shell (panel + taskbar query, no launcher) self-test passed"
  "[wm30] window chrome (maximize/restore/minimize/close via WM_ACTION_PIPE) self-test passed"
  "[clipboard] SYS_clipboard_set/get self-test passed."
  "[vfs] SYS_writefile/SYS_readfile self-test passed."
  "[settings] WM_SETTINGS_PIPE background-color self-test passed."
  "[wm36] confirm_close opt-in (WM_EVENT_CLOSE_REQUEST via WM_ACTION_PIPE) self-test passed"
  "[wm38] drop shadow + WM_SETTINGS_PIPE accent-color self-test passed"
  "[m42] bottom taskbar (Start button, running-app button, tray, maximize clamp, launcher toggle) self-test passed"
  "[m43] window snapping (left/right half, buffer-clamped) and the launcher overlay self-test passed"
  "[m44] wallpaper gradient, taskbar translucency over it, and the settings query round trip self-test passed"
  "[m45] SYS_taskinfo naming, WM_ACTION_KILL forcing a confirm_close client"
  "[m46] circular titlebar buttons, focus-gated glyphs, the deeper focused"
  "[m47] settings.conf round trip (including a corrupted one falling back to"
  "[m48] toast raised, still up mid-life, gone by its own deadline, and each"
  "[m49] the shared shortcut table resolving every chord (and refusing every"
  "[m50] 24 shm create/free cycles frame-neutral, a double free refused, 16"
  "[m51] z-order raise-on-click, occlusion-correct hit-testing (the overlap"
  "[m52] a ring-3 null dereference killing only its own task (exit 139), its"
  "[m53] directories created, entered, grown past one block, listed and read"
  "[m54] every task slot and every frame returned across"
  "[m55] a compositor SIGKILLed out from under two live clients, replaced, and"
  "[m56] SYS_unlink returning every block it freed and SYS_rename moving none,"
  "[m58] a resolution change carrying the whole desktop with it - panels"
  "[m59] descriptors (open/lseek/read/write/close), a 200 KiB file through"
  "[m60] a real argument vector (cp with two arguments), a command line with"
  "[m61] a minimize animating toward the taskbar - endpoints plus an intermediate"
  "[m62] the PC speaker gated on and off by its own deadline, muted when the volume"
  "[m63] SSE state preserved across task switches, a libc subset checked against"
  "[m63] icons are files: the desktop wrote them out, an edited palette entry"
  "[m63] four virtual desktops: a window hidden by switching away, back when"
  "[m64] the network reached user space: a DHCP lease rather than a"
  "[m65] capabilities: a manifest the kernel applies rather than a launcher,"
  "[m66] TCP: a handshake, 16 KiB through a 4 KiB buffer arriving byte for"
  "[m67] a preemptible kernel: \`int 0x80\` is a trap gate, four concurrent"
  "[m69] input-to-photon:"
  "[m70] the kernel log is readable from user space:"
  "[m71] files worth trusting:"
  "[m72] a script is a program:"
  "[m86] a shell that is a shell: a function called with a quoted argument,"
  # M89. Required rather than optional, because run-tests.sh installs the
  # port before this runs - so an image reaching here without /bin/toybox
  # is a broken build step rather than a legitimate configuration, and
  # the "skipped" line the kernel prints in that case is not this marker.
  "[m89] somebody else's userland: find, xargs, grep, sort and uniq"
  # M94. Required for the same reason [m89] is: run-tests.sh installs
  # what the compiler produced before this runs, so an image reaching
  # here without /bin/gcctest is a broken build step - unless the
  # compiler itself was never built, which gcc-test.sh says out loud and
  # which is why this marker is checked by name rather than by prefix.
  "[m94] a target this compiler knows by name"
  # M95. Two lines, because "it ran" and "there is one copy of the
  # library" are two claims and the second is the one the milestone is
  # actually about.
  "[m95] code that is loaded, not linked:"
  "shared library pages held:"
  # M99's second increment. A separate marker from [m95] because it is a
  # separate claim: [m95] says a program can open a library, this says
  # the loader can carry an interpreter - twenty-four objects at once,
  # every one named by a path rather than found by a search.
  "[m99ld] a loader an interpreter can use:"
  # M94's own bar, and a separate marker so that "the compiler works" and
  # "somebody else's project builds and runs" are two claims in the log
  # rather than one sentence that could lose half of itself.
  "[m94] somebody else's project: bzip2, built with"
  # M100's first bullet: the library stack, in dependency order. This is
  # the first entry that is a LIBRARY rather than a program, and the
  # marker says so - what it grades is somebody else's test program
  # passing here, not this project's opinion of somebody else's library.
  "[m100] the first library of the stack:"
  # M100's second increment, and a separate marker because it is a
  # sharper claim than [m100] is. zlib's own test checks its answers
  # against itself; libjpeg ships REFERENCE OUTPUT, so the seven
  # comparisons behind this marker are byte equality with pictures the
  # IJG's own encoder and decoder produced in 1995. libpng is here too,
  # and it is the first library in the stack that links the one before
  # it - `-lz`, out of the sysroot.
  "[m100b] two more libraries, graded by their own suites:"
  # M100's fourth increment. freetype has no suite of its own this
  # machine can run, so it is graded differentially like sh and libm:
  # the same fixture built for the host against the same freetype source,
  # and 570 glyph bitmaps that have to hash identically. expat brings its
  # own 4,392 checks. One marker for both, because they landed together
  # and the sentence names both.
  "[m100c] freetype against the host, and expat by its own suite:"
  # M100's fifth increment: sqlite, and it is the marker behind the most
  # kernel work in the stack - popen in libc and fcntl record locks in
  # the kernel both exist because this transcript would not match
  # without them.
  "[m100d] sqlite against the host:"
  # M100's sixth increment: harfbuzz, the first C++ library in the
  # stack, graded like freetype and sqlite - the same fixture against the
  # host's build - and through hb-ft, so the marker also says the two
  # libraries agree with each other.
  "[m100e] harfbuzz against the host:"
  "[fd] the redirect cycle (park stdout, point fd 1 at a file, write, restore)"
  "[m73] names, not numbers:"
  "[m74] the session remembers:"
  "[m75] environment and a place to stand:"
  "[m76] a signal a program can catch:"
  # M99: and the half M76 excluded - a fault, delivered to a handler the
  # faulting program installed. Three different signals from three
  # different vectors, which is what M52's note said this project could
  # not tell apart.
  "[m99] a fault a program can catch:"
  "[m77] POSIX names for what is already here:"
  "[q5] every syscall told a lie:"
  "[m78] memory that can be given back:"
  "[m79] two threads, one address space:"
  # M96: the two things that make M79's threads usable by a C runtime.
  "[m96] a thread with its own variables, and a wait that costs nothing:"
  # M97: C++, in four parts that fail for four different reasons - the
  # ABI runtime, the standard library on top of it, an exception across a
  # shared-object boundary, and GCC's own tests compiled unmodified. All
  # four are required for the same reason M94's and M95's are: the
  # toolchain is optional to BUILD and the image either has these
  # programs or it does not, and run-tests.sh is what puts them there.
  "[m97] C++ that throws:"
  "[m97] and the standard library on top of it:"
  "[m97] and across a shared object:"
  "[m97] and C++ nobody here wrote:"
  # M98: the machine's own binutils, on their own output. Required on
  # [m94]'s reasoning: run-tests.sh installs the native tools before
  # this runs, so an image reaching here without /bin/as is a broken
  # build step - unless the native toolchain was never built, which
  # install-native-toolchain.sh says out loud.
  "[m98] binutils runs here:"
  # M99: somebody else's language, on the same terms - optional to build,
  # installed by run-tests.sh before this runs, and the marker is the
  # sentence rather than a prefix so a shortened one is a failure. The
  # last clause of it is the one M80 could not have printed at all:
  # `import json` is a file read off this disk.
  "[m99] somebody else's language runs here:"
  # M106: the cores. Required on every boot, one core or four - on one
  # it is the serial baseline the four-core number is a ratio against,
  # and it is the only place MAX_TASKS' and MAX_FDS' high-water marks are
  # reported at all.
  "[m106] cores this machine can use:"
  # M105: the journal's two conditions. The marker is required rather
  # than the numbers alone because the numbers pass a boot where the
  # writers never ran - the sentence only prints when four of them did,
  # the scan found nothing, and every free block came back.
  "[m105] the journal's two conditions, measured together:"
  "[m81] a filesystem that can hold somebody else's program:"
  "[m82] a page that arrives when it is asked for:"
  "[m83] two processes from one:"
  "[m84] a program that replaces itself:"
  "[q9] a machine that runs out of things and stays up:"
  "[q16] devices that fail, and a machine that keeps running:"
  "[m85] a terminal that is a device:"
  "ptytest: all eight checks passed"
  "[m87] files with a type and a place:"
  "[m90] more than a gigabyte:"
  "[m91] an address space that is a set of mappings:"
  # M91 (second attempt): the marker is one line, so what proves the file
  # half landed is a second entry naming it rather than a longer first
  # one - a marker list that matched a prefix would keep passing if the
  # sentence lost its tail.
  "a file mapped MAP_PRIVATE reading back as its own bytes"
  "the same file mapped MAP_SHARED twice as one piece of memory"
  "[m92] a disk worth reading:"
  "[m93] a filesystem that can hold a source tree:"
  # M101: five markers rather than one, because they fail independently.
  # The sampler working says nothing about the syscall counters, and both
  # can be right while the VFS close path this milestone had to add is
  # still broken - which is the bug it found rather than the thing it
  # built, so it gets a marker of its own.
  "[m101] sampling profiler:"
  "[m101] per-syscall accounting:"
  "[m101] /proc survived 24 open/close cycles"
  "[m101] /proc/profile and /proc/syscalls both answer."
  "[m101] ring-3 half passed:"
  "[m101] the report above is /bin/profile's"
  # M102: two markers. The first is the kernel's own OOM line, which
  # proves the kill happened and names the victim; the second is the
  # verdict, which proves the machine was still there to print it.
  "[oom] out of physical memory filling"
  "[m102] out of memory, twice:"
  "[m68] wait queues: a task in SYS_waitfds is TASK_BLOCKED rather than"
  "[m40] boot-task fd reset self-test passed"
  "[m40] SYS_spawn failure-path self-test passed"
  "[smp] self-test passed."
  "[net] ICMP echo request/reply self-test passed"
  "[init] PID 1 spawned"
)

pass=1

if grep -qF "*** KERNEL PANIC:" "$LOG"; then
  pass=0
  echo "FAIL: kernel panicked - $(grep -F '*** KERNEL PANIC:' "$LOG" | head -1)"
fi

# M61: the frame budget, asserted by its silence. compositor.c counts
# animation frames that overrun FRAME_BUDGET_MS and prints one line at the
# end of a run if any did - a run that met its deadline says nothing at
# all. "An animation that stutters is worse than none", made into
# something a harness can fail on rather than something you have to watch
# for.
#
# M103: and it is asserted only on the default interrupt path. The frame
# budget is 16 ms of WALL CLOCK measured inside the guest, and routing
# every line through the I/O APIC makes QEMU emulate this machine at
# roughly half speed - kernel/dev/fwcfg.h has the numbers, including the
# one that shows the guest's own instruction stream is unchanged
# (syscall_null_cycles does not move while every wall figure doubles).
# Failing here on that run would be failing the compositor for something
# the compositor did not do; ignoring it silently would be worse. So it
# is reported, and says which.
if [ "${LEANOS_IOAPIC:-0}" = "1" ] &&
   grep -qF "[wm] animation missed its frame budget" "$LOG"; then
  echo "NOTE: $(grep -F '[wm] animation missed its frame budget' "$LOG" | head -1)"
  echo "      - not graded on the I/O APIC path: the budget is wall-clock and"
  echo "        this emulator runs at about half speed with an enabled APIC."
elif grep -qF "[wm] animation missed its frame budget" "$LOG"; then
  pass=0
  echo "FAIL: $(grep -F '[wm] animation missed its frame budget' "$LOG" | head -1)"
fi

# ---- Q6: the measurements, graded ------------------------------------
#
# The kernel emits `[perf] name value unit` for everything it measures
# (see klog_perf in kernel/kernel.c). tests/budgets.tsv gives each name a
# ceiling and the commit it was measured at. Until Q6 these numbers were
# printed and nothing read them, so the disk could have got a hundred
# times slower with every test still green.
#
# A measurement with no budget row is reported, not failed: a new
# measurement should be visible immediately and should not break the build
# before anyone has had a chance to choose its ceiling.
mkdir -p build
if [ ! -f build/perf-history.tsv ]; then
  printf 'when\tcommit\tmeasurement\tvalue\tunit\tceiling\n' > build/perf-history.tsv
fi
BUDGETS="tests/budgets.tsv"
perf_lines="$(grep -aoE '^\[perf\] [a-z0-9_]+ [0-9]+ [a-z]+' "$LOG" || true)"
if [ -n "$perf_lines" ]; then
  echo
  echo "Measurements this boot (ceiling from $BUDGETS):"
  while read -r _tag name value unit; do
    [ -n "${name:-}" ] || continue
    row="$(awk -F'\t' -v n="$name" '$1 == n {print; exit}' "$BUDGETS" 2>/dev/null || true)"
    if [ -z "$row" ]; then
      printf '  %-28s %10s %-3s  (no budget yet - add a row to %s)\n' \
        "$name" "$value" "$unit" "$BUDGETS"
      continue
    fi
    ceiling="$(printf '%s' "$row" | cut -f2)"
    measured="$(printf '%s' "$row" | cut -f4)"
    at="$(printf '%s' "$row" | cut -f5)"
    if [ "$value" -gt "$ceiling" ] 2>/dev/null; then
      pass=0
      printf '  %-28s %10s %-3s  OVER BUDGET (ceiling %s, measured %s at %s)\n' \
        "$name" "$value" "$unit" "$ceiling" "$measured" "$at"
    else
      printf '  %-28s %10s %-3s  ok (ceiling %s, was %s at %s)\n' \
        "$name" "$value" "$unit" "$ceiling" "$measured" "$at"
    fi
    # Q6: every measurement, every run. A threshold cannot see a trend -
    # ten commits each 5%% slower pass every check.
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
      "$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" \
      "perf:$name" "$value" "$unit" "$ceiling" >> build/perf-history.tsv
  done <<< "$perf_lines"
fi

missing=()
for marker in "${REQUIRED_MARKERS[@]}"; do
  if ! grep -qF "$marker" "$LOG"; then
    missing+=("$marker")
  fi
done

if [ "${#missing[@]}" -gt 0 ]; then
  pass=0
  echo "FAIL: ${#missing[@]}/${#REQUIRED_MARKERS[@]} required boot markers never appeared (log capture ended too early, or a real regression - try a longer SECONDS first):"
  for marker in "${missing[@]}"; do
    echo "  - $marker"
  done
fi

# Q6: one row per run, so a trend is visible rather than only a threshold.
# The boot prints its own duration; this records it next to the commit that
# produced it, which is the difference between "is this slow" and "when did
# it get slow".
mkdir -p build
boot_secs="$(sed -n 's/.*reached the desktop handoff in \([0-9][0-9]*\) s.*/\1/p' "$LOG" | tail -1)"
if [ ! -f build/test-history.tsv ]; then
  printf 'when\tcommit\tharness\tverdict\twall_s\tboot_s\n' > build/test-history.tsv
fi
printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  "$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" \
  "serial" \
  "$([ "$pass" -eq 1 ] && echo pass || echo fail)" \
  "$elapsed" \
  "${boot_secs:-}" >> build/test-history.tsv

# The log is kept when the caller named it: LEANOS_SERIAL_LOG is how a
# person debugging a failed marker gets to see what the machine actually
# said, and deleting the file they asked for would be the one thing that
# makes this harness hard to use.
[ -n "${LEANOS_SERIAL_LOG:-}" ] || rm -f "$LOG"
rm -f "$OVMF_VARS_RUNTIME"

if [ "$pass" -eq 1 ]; then
  echo "PASS: ${#REQUIRED_MARKERS[@]}/${#REQUIRED_MARKERS[@]} required boot markers found, no kernel panic."
  exit 0
fi
exit 1
