#!/usr/bin/env bash
# M189: the log a machine with no serial port leaves on the stick it booted
# from. Boots an image made by tools/make-hardware-image.sh over usb-storage,
# twice, and requires what tools/read-usb-log.py reads back off the image to
# be what the serial port carried - then boots a third time and has the
# kernel panic once the desktop is up (M200), and requires the panic to be in
# the log on the stick - then a fourth time with lean_os.cfg pointing at
# sectors that are NOT the log area, and requires the kernel to refuse and
# the sectors to come back unchanged.
#
# The serial port is the oracle here and only here: it is the channel the
# log file exists to replace on a machine that does not have one.
set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE="build/os-image-usblog.bin"
WORK="build/usb-log-test"
OVMF_CODE="build/ovmf/OVMF_CODE.fd"
OVMF_VARS_TEMPLATE="build/ovmf/OVMF_VARS.fd"
BOOT_SECONDS=${USB_LOG_BOOT_SECONDS:-240}
DESKTOP_MARKER="[init] PID 1 spawned"

mkdir -p "$WORK"
failures=0
fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }
pass() { echo "ok   $*"; }

[ -f build/os-image.bin ] || { echo "no build/os-image.bin - run 'make all' first" >&2; exit 1; }
tools/make-hardware-image.sh --video 1024x768 --output "$IMAGE" > "$WORK/make-image.log"
LOG_OPTION=$(grep -E '^ *log=' "$WORK/make-image.log" | tr -d ' ')
[ -n "$LOG_OPTION" ] || { echo "make-hardware-image.sh wrote no log= line" >&2; exit 1; }
LOG_LBA=${LOG_OPTION#log=}; LOG_LBA=${LOG_LBA%+*}

boot() {
  local serial="$1" qemu_pid waited=0 extra="${2:-}"
  cp "$OVMF_VARS_TEMPLATE" "$WORK/vars.fd"
  : > "$serial"
  qemu-system-x86_64 -m 1024 -smp 1 -display none \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$WORK/vars.fd" \
    -device qemu-xhci,id=xhci0 \
    -drive "if=none,id=disk0,format=raw,file=$IMAGE" \
    -device usb-storage,bus=xhci0.0,drive=disk0 \
    ${extra:+-fw_cfg "$extra"} \
    -serial "file:$serial" -monitor none &
  qemu_pid=$!
  local until="$DESKTOP_MARKER"
  [ -n "$extra" ] && until="KERNEL PANIC"
  while ! grep -qF "$until" "$serial" 2>/dev/null; do
    if ! kill -0 "$qemu_pid" 2>/dev/null || [ "$waited" -ge "$BOOT_SECONDS" ]; then
      kill "$qemu_pid" 2>/dev/null || true
      wait "$qemu_pid" 2>/dev/null || true
      return 1
    fi
    sleep 1
    waited=$((waited + 1))
  done
  # Three of the writer's one-second intervals, so the marker is on the disk.
  sleep 4
  echo "marker written at $(date +%T)" >> "$WORK/timeline"
  kill "$qemu_pid"
  wait "$qemu_pid" 2>/dev/null || true
}

tr_serial() { tr -d '\r' < "$1"; }

echo "Boot 1 of 4 over usb-storage..."
boot "$WORK/serial-1.log" || fail "boot 1 did not reach '$DESKTOP_MARKER' within ${BOOT_SECONDS}s"
tools/read-usb-log.py "$IMAGE" > "$WORK/disk-1.log" 2>/dev/null || fail "read-usb-log.py could not read the image after boot 1"

grep -q "^\[disk-log\] mirroring the kernel log to LBA $LOG_LBA " <(tr_serial "$WORK/serial-1.log") \
  && pass "the kernel found the log area at the LBA lean_os.cfg named" \
  || fail "the serial log has no '[disk-log] mirroring ... LBA $LOG_LBA'"
grep -qx "===== lean_os boot 1 =====" "$WORK/disk-1.log" \
  && pass "the first boot is recorded as boot 1" || fail "no '===== lean_os boot 1 =====' in the disk log"
grep -qF "$DESKTOP_MARKER" "$WORK/disk-1.log" \
  && pass "the disk log reaches the desktop marker" || fail "the disk log does not contain '$DESKTOP_MARKER'"

# Every serial line up to the desktop marker must be in the disk log, in
# order - not a sample of them. The serial copy is the one with \r\n.
python3 - "$WORK/serial-1.log" "$WORK/disk-1.log" <<'PYTHON' && pass "every serial line to the desktop is in the disk log, in order" || fail "the disk log is not the serial log"
import sys
serial = open(sys.argv[1], "rb").read().replace(b"\r\n", b"\n")
import re
# M193: the file carries a timestamp line a second that the serial port does
# not - "[t+12.3s] disk read ..." - and those are the only lines allowed to
# be in one and not the other.
disk = re.sub(rb"(?m)^\[t\+[0-9]+\.[0-9]s\] disk read [^\n]*\n", b"",
              open(sys.argv[2], "rb").read())
# M197: and every kernel line starts with the time it was written, which the
# serial copy does not carry either. The times must never run backwards.
times = [float(t) for t in re.findall(rb"(?m)^\[ *([0-9]+\.[0-9]{3})\] ", disk)]
if len(times) < 100:
    sys.exit("only %d kernel lines carry a time in the disk log" % len(times))
if any(b < a for a, b in zip(times, times[1:])):
    sys.exit("the disk log's line times run backwards")
disk = re.sub(rb"(?m)^\[ *[0-9]+\.[0-9]{3}\] ", b"", disk)
start = serial.find(b"[disk-log] mirroring")
end = serial.find(b"[init] PID 1 spawned")
if start < 0 or end < 0:
    sys.exit("markers missing from the serial log")
# The firmware and the boot loader print before the kernel exists, and the
# kernel's log starts at its own first line.
kernel_start = serial.find(b"lean_os kernel:")
wanted = serial[kernel_start:end]
if wanted not in disk:
    at = 0
    while at < len(wanted) and wanted[:at + 256] in disk:
        at += 256
    sys.exit("disk log diverges from serial within 256 bytes of offset %d: %r"
             % (at, wanted[at:at + 256]))
PYTHON

# M202: a busy second says where the kernel's time went, and the reader
# names what it says. Booting is busy enough to produce one; a line that
# still carries sys#N after reading means the syscall table was not found.
grep -qE "^\[t\+[0-9]+\.[0-9]s\] .*; kernel samples [0-9]+ in " "$WORK/disk-1.log" \
  && pass "a busy second's stamp names where the kernel's time went" \
  || fail "no stamp line in the disk log carries '; kernel samples N in'"
if grep -q "kernel samples.*sys#[0-9]" "$WORK/disk-1.log"; then
  fail "read-usb-log.py left a system call as sys#N rather than naming it"
else
  pass "every system call in the profile is named"
fi

echo "Boot 2 of 4..."
boot "$WORK/serial-2.log" || fail "boot 2 did not reach the desktop"
tools/read-usb-log.py "$IMAGE" > "$WORK/disk-2.log" 2>/dev/null || true
tools/read-usb-log.py "$IMAGE" --last > "$WORK/disk-2-last.log" 2>/dev/null || true
grep -qx "===== lean_os boot 1 =====" "$WORK/disk-2.log" && grep -qx "===== lean_os boot 2 =====" "$WORK/disk-2.log" \
  && pass "a second boot appends rather than starting the file over" \
  || fail "boot 2's log does not hold both boots"
if grep -qx "===== lean_os boot 2 =====" "$WORK/disk-2-last.log" && ! grep -qx "===== lean_os boot 1 =====" "$WORK/disk-2-last.log"; then
  pass "--last gives the most recent boot alone"
else
  fail "--last did not isolate boot 2"
fi

echo "Boot 3 of 4, asked to panic once the desktop is up..."
boot "$WORK/serial-panic.log" "name=opt/leanos/panictest,string=1" || fail "boot 3 did not panic when asked to"
tools/read-usb-log.py "$IMAGE" --last > "$WORK/disk-panic.log" 2>/dev/null || true
grep -qF "*** KERNEL PANIC: the harness asked for this panic" "$WORK/disk-panic.log" \
  && pass "a panic's own message reaches the log on the stick" \
  || fail "the panic is not in the stick's log - the machine went down with its last words on the screen"
grep -qx "===== lean_os boot 3 =====" "$WORK/disk-panic.log" \
  && pass "and it is recorded as the boot it happened in" || fail "the panicking boot is not boot 3 in the log"

echo "Boot 4 of 4, with lean_os.cfg naming sectors that are not the log area..."
ESP_START_LBA=$(make -s print-esp-start-lba)
WRONG_LBA=$((LOG_LBA - 2048))
printf 'video=1024x768\nlog=%d+49152\n' "$WRONG_LBA" > "$WORK/wrong.cfg"
mcopy -i "$IMAGE@@$((ESP_START_LBA * 512))" -o "$WORK/wrong.cfg" ::EFI/BOOT/lean_os.cfg
dd if="$IMAGE" of="$WORK/before.bin" bs=512 skip="$WRONG_LBA" count=2048 2>/dev/null
boot "$WORK/serial-3.log" || fail "boot 4 did not reach the desktop"
dd if="$IMAGE" of="$WORK/after.bin" bs=512 skip="$WRONG_LBA" count=2048 2>/dev/null
grep -q "^\[disk-log\] LBA $WRONG_LBA does not start with the log area's header" <(tr_serial "$WORK/serial-3.log") \
  && pass "a log= that names the wrong sectors is refused, and says so" \
  || fail "no refusal for LBA $WRONG_LBA in the serial log"
cmp -s "$WORK/before.bin" "$WORK/after.bin" \
  && pass "the sectors it was pointed at are unchanged" \
  || fail "the kernel wrote to sectors that were not the log area"

if [ "$failures" -ne 0 ]; then
  echo "usb-log-test: $failures failed (logs in $WORK)" >&2
  exit 1
fi
echo "usb-log-test: all passed"
