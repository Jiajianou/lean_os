#!/usr/bin/env bash
# tools/window-test.sh - M116: the window a person actually gets.
#
# Every other instrument in this tree looks at the guest: its serial log,
# its framebuffer, its disk. None of them has ever looked at the one
# thing between the guest and a person's eyes - the window QEMU opens on
# the host - and on 2026-09-10 that window shrank to half its size while
# every test stayed green. Homebrew had upgraded QEMU to 11.1, whose cocoa
# display sizes the window in PHYSICAL pixels on a Retina screen, so the
# same 1024x768 desktop opened at 512x384 points. The guest was exactly
# as it had always been, which is why nothing that grades the guest could
# have seen it, and why it read to the person using it as "the screen got
# small during the browser work".
#
# So this boots the machine the way a person does - tools/run-qemu.sh,
# with its real display and nothing added but -snapshot (so the disk is
# not written), a serial log to know when the desktop is up and a monitor
# to ask the guest its framebuffer size - and measures the window with
# CGWindowList (tools/window-size.swift). The rule is the one a person
# would state: one guest pixel is one point, so the window's content is
# at least as wide as the guest's screen, in points.
#
# macOS only, because cocoa is macOS's display and the scaling question
# does not exist elsewhere; it says so and exits 0 on another host, or in
# a session with no window server (over ssh), rather than pretending to
# have measured something. It opens a window for about fifteen seconds.
set -uo pipefail
cd "$(dirname "$0")/.."

if [ "$(uname)" != "Darwin" ]; then
  echo "window-test: not macOS - there is no cocoa window to measure here (skipped)"
  exit 0
fi
if ! launchctl managername 2>/dev/null | grep -q Aqua; then
  echo "window-test: no window server in this session (ssh?) - nothing to measure (skipped)"
  exit 0
fi
if ! command -v swift >/dev/null; then
  echo "window-test: needs swift, from the Xcode command line tools that also provide clang" >&2
  exit 1
fi

DIR=$(mktemp -d /tmp/leanos-window-XXXXXX)
TAG="leanos-window-test-$$"
LOG="$DIR/serial.log"
MON="$DIR/mon.sock"
cleanup() {
  pkill -f "qemu-system-x86_64.*$TAG" 2>/dev/null
  rm -rf "$DIR"
}
trap cleanup EXIT

# QEMU_HIDPI is the opt-out; the default is what is being graded.
QEMU_HIDPI=0 ./tools/run-qemu.sh -- -snapshot -name "$TAG" \
  -serial "file:$LOG" -monitor "unix:$MON,server,nowait" \
  > "$DIR/run.out" 2>&1 &

deadline=$(( $(date +%s) + 300 ))  # run-qemu.sh builds first
until grep -q "\[init\] PID 1 spawned" "$LOG" 2>/dev/null; do
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "window-test: the machine never reached the desktop" >&2
    tail -20 "$DIR/run.out" >&2
    exit 1
  fi
  sleep 1
done
sleep 4 # the compositor's first frame, and any mode change it makes

QPID=$(pgrep -f "qemu-system-x86_64.*$TAG" | head -1)
if [ -z "$QPID" ]; then
  echo "window-test: no QEMU process for this run" >&2
  exit 1
fi

# The guest's framebuffer size, from the guest - a screendump's PPM
# header - rather than assumed, so a QEMU_RES or a saved mode is
# measured against what it is and not against 1024x768.
python3 - "$MON" "$DIR/shot.ppm" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
s.sendall(("screendump %s\n" % sys.argv[2]).encode())
time.sleep(1.5)
EOF
read -r GW GH < <(head -c 64 "$DIR/shot.ppm" | tr '\n' ' ' | awk '{print $2, $3}')
if [ -z "${GW:-}" ]; then
  echo "window-test: could not read the guest's framebuffer size" >&2
  exit 1
fi

if ! SIZE=$(swift tools/window-size.swift "$QPID"); then
  echo "window-test: QEMU (pid $QPID) has no window on screen" >&2
  exit 1
fi
read -r WW WH <<< "$SIZE"

echo "window-test: guest ${GW}x${GH} pixels, window ${WW}x${WH} points"
# The height includes the title bar, so it is at least the guest's.
if [ "$WW" -lt "$GW" ] || [ "$WH" -lt "$GH" ]; then
  echo "window-test: FAIL - the window is smaller than the guest's screen." >&2
  if [ "$WW" -le $(( GW / 2 + 2 )) ]; then
    echo "  Half the width: QEMU is drawing one guest pixel per PHYSICAL pixel" >&2
    echo "  on a Retina display. See the M116 note in tools/run-qemu.sh." >&2
  fi
  exit 1
fi
echo "window-test: pass - one guest pixel is at least one point"
