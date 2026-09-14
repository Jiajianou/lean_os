#!/usr/bin/env bash
set -euo pipefail

SECONDS_TO_RUN="${1:-900}"
shift || true
EXTRA_ARGS=("$@")

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
OVMF_VARS_RUNTIME="$(mktemp -t qemu-serial-ovmf-vars-XXXXXX.fd)"
IOAPIC_FWCFG=""
if [ "${LEANOS_IOAPIC:-0}" = "1" ]; then
  IOAPIC_FWCFG="-fw_cfg name=opt/leanos/ioapic,string=1"
fi

NIC_STREAM_BYTES=262144
NIC_STREAM="$(mktemp -t leanos-nicstream-XXXXXX)"
python3 -c "
import sys
n = int(sys.argv[2])
sys.stdout = open(sys.argv[1], 'wb')
sys.stdout.write(bytes(((i * 7 + (i >> 9)) & 0xFF) for i in range(n)))
" "$NIC_STREAM" "$NIC_STREAM_BYTES"
NETDEV="user,id=net0,guestfwd=tcp:10.0.2.100:7777-cmd:cat $NIC_STREAM"

LOG="${LEANOS_SERIAL_LOG:-$(mktemp -t qemu-serial-XXXXXX.log)}"
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

QEMU_MEM=${QEMU_MEM:-4096}
QEMU_CPUS=${QEMU_CPUS:-1}
case "${QEMU_DISK:-virtio}" in
  ide)
    DISK_ARGS=(-drive "format=raw,snapshot=on,file=$IMAGE")
    ;;
  ahci)
    DISK_ARGS=(-device ich9-ahci,id=ahci0
               -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE"
               -device ide-hd,drive=disk0,bus=ahci0.0)
    ;;
  nvme)
    DISK_ARGS=(-drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE"
               -device nvme,drive=disk0,serial=leanos0)
    ;;
  *)
    DISK_ARGS=(-drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE"
               -device virtio-blk-pci,drive=disk0)
    ;;
esac
USB_ARGS=(-device qemu-xhci,id=xhci0
          -device usb-kbd,bus=xhci0.0
          -device usb-mouse,bus=xhci0.0)
if [ "${QEMU_USB:-1}" = "0" ]; then
  USB_ARGS=()
fi

qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  "${DISK_ARGS[@]}" -display none \
  -netdev "$NETDEV" -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  "${USB_ARGS[@]}" \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  -fw_cfg "name=opt/leanos/nicstream,string=$NIC_STREAM_BYTES" \
  $IOAPIC_FWCFG \
  -serial file:"$LOG" -monitor none ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

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
  "[m89] somebody else's userland: find, xargs, grep, sort and uniq"
  "[m94] a target this compiler knows by name"
  "[m95] code that is loaded, not linked:"
  "shared library pages held:"
  "[m99ld] a loader an interpreter can use:"
  "[m94] somebody else's project: bzip2, built with"
  "[rng] a random device that is not a counter:"
  "[m100] the first library of the stack:"
  "[m100b] two more libraries, graded by their own suites:"
  "[m100c] freetype against the host, and expat by its own suite:"
  "[m100d] sqlite against the host:"
  "[m100e] harfbuzz against the host:"
  "[m100f] TLS end to end over M66's TCP:"
  "[m100g] mbedtls's own suites:"
  "[fd] the redirect cycle (park stdout, point fd 1 at a file, write, restore)"
  "[m73] names, not numbers:"
  "[m74] the session remembers:"
  "[m75] environment and a place to stand:"
  "[m76] a signal a program can catch:"
  "[m99] a fault a program can catch:"
  "[m77] POSIX names for what is already here:"
  "[q5] every syscall told a lie:"
  "[m78] memory that can be given back:"
  "[m79] two threads, one address space:"
  "[m96] a thread with its own variables, and a wait that costs nothing:"
  "[m97] C++ that throws:"
  "[m97] and the standard library on top of it:"
  "[m97] and across a shared object:"
  "[m97] and C++ nobody here wrote:"
  "[m98] binutils runs here:"
  "[m99] somebody else's language runs here:"
  "[m106] cores this machine can use:"
  "[m105] the journal's two conditions, measured together:"
  "[m81] a filesystem that can hold somebody else's program:"
  "[m82] a page that arrives when it is asked for:"
  "[m83] two processes from one:"
  "[m84] a program that replaces itself:"
  "[q9] a machine that runs out of things and stays up:"
  "[m111] GNU grep 3.11, built here, installed by \`os\` and run"
  "[m111] a package binary named \`compositor\` gets 0x0, not"
  "[m111] the kernel's package registry:"
  "[m111] a package manager: GNU grep 3.11, built here by"
  "[m112] the Files app's tree walks, on leanfs:"
  "[m100h] what porting a browser added:"
  "[m113] the browser is installed:"
  "[m114] more than one nameserver:"
  "[m116] a stream from the host:"
  "[m117] a desktop that sleeps and a click that shows:"
  "[m118] AF_UNIX: a socketpair both ways,"
  "[m119] a message pump:"
  "[m120] a buffer shared across a channel:"
  "[m125] lvgl rendered"
  "[m125] a third-party toolkit on this compositor:"
  "[m127] settings rendered"
  "[m127] task_manager rendered"
  "[m127] the desktop's own applications on LVGL:"
  "[m121] a second compiler that knows this OS by name:"
  "ONE PROGRAM FROM TWO COMPILERS"
  "[m137] a third language for this target:"
  "[m138] the Rust standard library on this machine:"
  "[m138] ruststd: "
  "[q16] devices that fail, and a machine that keeps running:"
  "[m85] a terminal that is a device:"
  "ptytest: all eight checks passed"
  "[m87] files with a type and a place:"
  "[m90] more than a gigabyte:"
  "[m91] an address space that is a set of mappings:"
  "a file mapped MAP_PRIVATE reading back as its own bytes"
  "the same file mapped MAP_SHARED twice as one piece of memory"
  "[m92] a disk worth reading:"
  "[m107] the devices a real machine has:"
  "[m93] a filesystem that can hold a source tree:"
  "[m101] sampling profiler:"
  "[m101] per-syscall accounting:"
  "[m101] /proc survived 24 open/close cycles"
  "[m101] /proc/profile and /proc/syscalls both answer."
  "[m101] ring-3 half passed:"
  "[m101] the report above is /bin/profile's"
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

[ -n "${LEANOS_SERIAL_LOG:-}" ] || rm -f "$LOG"
rm -f "$OVMF_VARS_RUNTIME"

if [ "$pass" -eq 1 ]; then
  echo "PASS: ${#REQUIRED_MARKERS[@]}/${#REQUIRED_MARKERS[@]} required boot markers found, no kernel panic."
  exit 0
fi
exit 1
