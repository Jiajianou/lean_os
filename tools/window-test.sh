#!/usr/bin/env bash
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

QEMU_HIDPI=0 ./tools/run-qemu.sh -- -snapshot -name "$TAG" \
  -serial "file:$LOG" -monitor "unix:$MON,server,nowait" \
  > "$DIR/run.out" 2>&1 &

deadline=$(( $(date +%s) + 300 ))
until grep -q "\[init\] PID 1 spawned" "$LOG" 2>/dev/null; do
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "window-test: the machine never reached the desktop" >&2
    tail -20 "$DIR/run.out" >&2
    exit 1
  fi
  sleep 1
done
sleep 4

QPID=$(pgrep -f "qemu-system-x86_64.*$TAG" | head -1)
if [ -z "$QPID" ]; then
  echo "window-test: no QEMU process for this run" >&2
  exit 1
fi

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
if [ "$WW" -lt "$GW" ] || [ "$WH" -lt "$GH" ]; then
  echo "window-test: FAIL - the window is smaller than the guest's screen." >&2
  if [ "$WW" -le $(( GW / 2 + 2 )) ]; then
    echo "  Half the width: QEMU is drawing one guest pixel per PHYSICAL pixel" >&2
    echo "  on a Retina display. See the M116 note in tools/run-qemu.sh." >&2
  fi
  exit 1
fi
echo "window-test: pass - one guest pixel is at least one point"
