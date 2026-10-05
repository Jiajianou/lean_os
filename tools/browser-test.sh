#!/usr/bin/env bash
set -uo pipefail

# Chromium's own browser, asked to draw a page.
#
# The graded battery reaches the browser 470 seconds into a boot, and what is
# being iterated on here is one program: so this boots the machine with
# opt/leanos/browser=1, which runs /bin/chromiumshell on one page and powers
# off. Nothing else in the battery runs.

cd "$(dirname "$0")/.."

IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
LOG="${LEANOS_BROWSER_LOG:-build/browser-serial.log}"
SECONDS_TO_RUN="${1:-900}"
# The page, from outside the image. Without one the kernel's own data: URL is
# what runs, which is the picture tools/browser-shot.py grades; with one this
# harness is how a real page - a resolver, a connection, a certificate - is
# asked for without editing a kernel constant and recreating the disk.
BROWSER_URL="${2:-${LEANOS_BROWSER_URL:-}}"
# The switches that are about the measurement rather than the machine. The
# kernel's own default is "--v=0 --single-process"; this is how the four-process
# arrangement the desktop runs - and the network service that is its own
# process in it - is asked for here, where every child's stderr is on the
# serial line.
BROWSER_ARGS="${LEANOS_BROWSER_ARGS:-}"
# Expanded below as ${URL_FWCFG[@]+"${URL_FWCFG[@]}"}: this Mac's bash is 3.2,
# where "${empty[@]}" under `set -u` is an unbound variable rather than
# nothing at all, and the script dies before QEMU starts.
URL_FWCFG=()
if [ -n "$BROWSER_URL" ]; then
  URL_FWCFG+=(-fw_cfg "name=opt/leanos/browserurl,string=$BROWSER_URL")
  echo "[harness] page: $BROWSER_URL"
fi
if [ -n "$BROWSER_ARGS" ]; then
  URL_FWCFG+=(-fw_cfg "name=opt/leanos/browserargs,string=$BROWSER_ARGS")
  echo "[harness] switches: $BROWSER_ARGS"
fi
QEMU_CPUS=${QEMU_CPUS:-1}
QEMU_MEM=${QEMU_MEM:-4096}

OVMF_CODE=build/ovmf/OVMF_CODE.fd
OVMF_VARS=build/ovmf/OVMF_VARS.fd
[ -f "$OVMF_CODE" ] || ./tools/build-ovmf.sh || exit 1
[ -f "$IMAGE" ] || { echo "browser-test: no image at $IMAGE - run make first" >&2; exit 1; }

# Whether the browser is on this image, read off the image on this side by
# the reader the interactive suite decides its skips with - so the machine's
# "[m169] ... not on this image - skipped" can be checked against something
# other than the machine. Until the kernel's switch read the program with a
# reader that may answer "absent", that line was unreachable and an image
# without a browser panicked instead; a skip is only a pass when the host
# agrees there was nothing to run. 0 present, 1 absent, 2 unreadable.
python3 tools/qemu_input.py has --image "$IMAGE" /bin/chromiumshell >/dev/null
ON_IMAGE=$?
# Absent is a skip only on a host that never built the browser. Where
# content_shell has been built - the path tools/install-browser.sh installs
# from - an image without it is M113's failure, not a valid image: a `make all`
# recreated the disk after the browser went on, and the skip would hide that
# the next person to touch the kernel gets a Browser icon that opens nothing.
# Refused before booting, with what puts it back.
SHELL_BIN="build/chromium/src/out/${LEANOS_CHROMIUM_OUT:-LeanOS}/content_shell"
if [ "$ON_IMAGE" -eq 1 ] && [ -f "$SHELL_BIN" ]; then
  echo "FAIL: this host has built the browser ($SHELL_BIN) and $IMAGE does not have"
  echo "      /bin/chromiumshell - the image was recreated after it was installed"
  echo "      (M113). make browser-if-built puts it back."
  exit 1
fi
case "$ON_IMAGE" in
  0) echo "[harness] /bin/chromiumshell is on $IMAGE" ;;
  1) echo "[harness] /bin/chromiumshell is not on $IMAGE - expecting the machine to say it skipped" ;;
  *) echo "[harness] could not read $IMAGE's filesystem here - trusting the machine's word on what it has" ;;
esac

OVMF_VARS_RUNTIME=build/ovmf/OVMF_VARS_browser.fd
cp "$OVMF_VARS" "$OVMF_VARS_RUNTIME"
: > "$LOG"

echo "[harness] Chromium's own browser drawing a page - ceiling ${SECONDS_TO_RUN}s"
qemu-system-x86_64 \
  -m "$QEMU_MEM" -smp "$QEMU_CPUS" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS_RUNTIME" \
  -drive "if=none,id=disk0,format=raw,snapshot=on,file=$IMAGE" \
  -device virtio-blk-pci,drive=disk0 -display none \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -audiodev none,id=snd0 -device AC97,audiodev=snd0 \
  -fw_cfg name=opt/leanos/selftest,string=1 \
  -fw_cfg name=opt/leanos/browser,string=1 \
  ${URL_FWCFG[@]+"${URL_FWCFG[@]}"} \
  -serial file:"$LOG" -monitor none &
QEMU_PID=$!
disown "$QEMU_PID" 2>/dev/null || true

deadline=$(( $(date +%s) + SECONDS_TO_RUN ))
while kill -0 "$QEMU_PID" 2>/dev/null; do
  if grep -q "\[browser\] done\|KERNEL PANIC" "$LOG" 2>/dev/null; then
    break
  fi
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "browser-test: the machine did not finish within ${SECONDS_TO_RUN}s" >&2
    break
  fi
  sleep 5
done
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

if grep -q "\[m169\] /bin/chromiumshell is not on this image" "$LOG"; then
  grep -E "^\[m169\]" "$LOG" | tail -2
  if [ "$ON_IMAGE" -eq 0 ]; then
    echo "FAIL: the machine says /bin/chromiumshell is not on this image, and it is - see $LOG"
    exit 1
  fi
  if grep -q "KERNEL PANIC" "$LOG"; then
    grep -E "KERNEL PANIC" "$LOG" | tail -2
    echo "FAIL: the machine skipped the browser and then panicked - see $LOG"
    exit 1
  fi
  echo "SKIP: this image has no browser on it."
  exit 0
fi
if [ "$ON_IMAGE" -eq 1 ]; then
  grep -E "KERNEL PANIC" "$LOG" | tail -2
  echo "FAIL: this image has no /bin/chromiumshell and the machine did not say it"
  echo "      skipped - a missing optional program has to be a skip, not a panic"
  echo "      or a hang (see $LOG)"
  exit 1
fi

grep -E "^\[m169\]" "$LOG" | tail -3

if grep -q "KERNEL PANIC" "$LOG"; then
  grep -E "^\[pmm\]|KERNEL PANIC" "$LOG" | tail -5
  echo "FAIL: the machine panicked while the browser was running - see $LOG"
  exit 1
fi
if ! grep -q "^\[browser\] done" "$LOG"; then
  echo "FAIL: the browser run never finished - see $LOG"
  exit 1
fi

# The picture, graded here rather than on the machine: a PNG decoder belongs
# on the side that can be told it is wrong by something else.
if [ -n "$BROWSER_URL" ] || [ -n "$BROWSER_ARGS" ]; then
  echo "      page was ${BROWSER_URL:-the compiled-in one} - the pixel grader below is for the"
  echo "      compiled-in page, so what to read is $LOG"
  python3 tools/browser-shot.py "$LOG" --save build/browser-shot.png >/dev/null 2>&1 \
    && echo "      the decoded picture is in build/browser-shot.png"
  exit 0
fi

python3 tools/browser-shot.py "$LOG" --save build/browser-shot.png
rc=$?
if [ $rc -ne 0 ]; then
  echo "      the decoded picture is in build/browser-shot.png"
  exit 1
fi
echo "      the picture is in build/browser-shot.png"
