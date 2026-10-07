#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

# M227. Electron, built by tools/build-chromium.sh out of the browser's own
# checkout with Electron's series on it:
#
#   LEANOS_CHROMIUM_SERIES=electron tools/build-chromium.sh electron
#
# This grades what that build produced and puts it on the image, in a
# directory of its own: Electron finds resources.pak, its locales and
# resources/default_app.asar beside the executable, and the browser's
# resources.pak is already in /bin. Whether it WORKS is the [m227] boot
# self-test's question, which runs tests/electron/app on the machine.

OUT="$ROOT/build/chromium/src/out/${LEANOS_CHROMIUM_OUT:-Electron}"
ELECTRON="$OUT/electron"
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"
IMAGE="${LEANOS_IMAGE:-$ROOT/build/os-image.bin}"
STRIPPED="$ROOT/build/electron.stripped"
DIRECTORY=/usr/lib/electron

PASS=0
FAIL=0
check() {
  if [ "$1" = "0" ]; then
    echo "electron-test: pass - $2"
    PASS=$((PASS + 1))
  else
    echo "electron-test: FAIL - $2" >&2
    FAIL=$((FAIL + 1))
  fi
}

if command -v node > /dev/null; then
  for f in tests/electron/app/*.js; do
    node --check "$f"
    check $? "$f parses, by the host's own node"
  done
fi

if [ ! -x "$ELECTRON" ]; then
  echo "electron-test: no $ELECTRON - build it with the line above; skipping the rest"
  echo "electron-test: $PASS passed, $FAIL failed"
  [ "$FAIL" = "0" ]
  exit
fi

# M223's two traps, which apply to every program linked against this libc
# out of Chromium's ninja: ninja does not track the sysroot's libc.a, and
# nothing copies build/libc.a there but make sysroot.
LIBC="$ROOT/build/sysroot/usr/lib/libc.a"
[ ! -f "$LIBC" ] || [ "$ELECTRON" -nt "$LIBC" ]
check $? "electron is newer than the sysroot's libc.a (rm it and rebuild if not - ninja will not)"
cmp -s "$ROOT/build/libc.a" "$LIBC"
check $? "the sysroot's libc.a is build/libc.a (run make sysroot-libc if not)"

HEADER=$("${PREFIX}readelf" -h "$ELECTRON")
grep -q 'Type: *EXEC' <<< "$HEADER" && grep -q 'Machine: *Advanced Micro Devices X86-64' <<< "$HEADER"
check $? "out/Electron/electron is an x86-64 ELF executable"

! "${PREFIX}readelf" -l "$ELECTRON" | grep -q INTERP
check $? "with no interpreter - one static file, the shape every program here has"

ENTRY=$(awk '/Entry point address/ {print $4}' <<< "$HEADER")
[ "$((ENTRY >= 0x8000000000))" = "1" ]
check $? "loaded at 512 GiB where this OS puts programs (entry $ENTRY)"

SYMBOLS=$("${PREFIX}nm" -C "$ELECTRON")

"$ROOT/tools/v8-builtins-isa.sh" "$ELECTRON"
check $? "V8's embedded builtins use nothing past SSE3 (patch 0062)"

# Electron's arrangement with Node: a thread waits on libuv's own epoll set
# from inside Chromium's message loop (node_bindings_linux.cc), which needs
# the epoll backend M226 gave libuv and a kernel that nests epoll sets.
grep -q ' uv__epoll_dispatching$' <<< "$SYMBOLS" &&
  grep -q 'electron::NodeBindingsLinux::PollEvents' <<< "$SYMBOLS"
check $? "libuv is the epoll backend, and NodeBindingsLinux waits on it"

# The Linux desktop libraries electron-port 0002 put behind their own flags.
# Asked as namespaces rather than words (M159): a binary is allowed to MENTION
# GTK in a string; it is not allowed to call it.
! grep -qE ' [TtUu] (gtk_|gdk_|g_dbus_|dbus_|notify_notification_|g_settings_)' <<< "$SYMBOLS"
check $? "no GTK, GDK, GIO D-Bus, libdbus or libnotify function is linked"

for asset in resources.pak locales/en-US.pak resources/default_app.asar; do
  [ -f "$OUT/$asset" ]
  check $? "the build produced $asset"
done

if [ ! -f "$IMAGE" ]; then
  echo "electron-test: no $IMAGE - run make, then this again"
else
  if [ ! -f "$STRIPPED" ] || [ "$ELECTRON" -nt "$STRIPPED" ]; then
    "${PREFIX}strip" -o "$STRIPPED" "$ELECTRON"
  fi
  check $? "stripped to $(( $(wc -c < "$STRIPPED") / 1024 / 1024 )) MB from $(( $(wc -c < "$ELECTRON") / 1024 / 1024 )) MB"
  make -s leanfs-put > /dev/null 2>&1
  build/leanfs-put "$IMAGE" "$STRIPPED" "$DIRECTORY/electron" > /dev/null
  check $? "installed as $DIRECTORY/electron"
  installed=0
  for asset in resources.pak chrome_100_percent.pak chrome_200_percent.pak \
               v8_context_snapshot.bin snapshot_blob.bin locales/en-US.pak \
               resources/default_app.asar; do
    if [ -f "$OUT/$asset" ]; then
      build/leanfs-put "$IMAGE" "$OUT/$asset" "$DIRECTORY/$asset" > /dev/null || exit 1
      installed=$((installed + 1))
    fi
  done
  check 0 "and $installed of its resources beside it"
  build/leanfs-put -r "$IMAGE" tests/electron/app /lib/electron-test/app > /dev/null
  check $? "and tests/electron/app as /lib/electron-test/app - the [m227] boot self-test runs it"
fi

echo "electron-test: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
