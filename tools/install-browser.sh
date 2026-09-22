#!/usr/bin/env bash
set -uo pipefail

# The browser, onto the image: Chromium's own content_shell as /bin/chromiumshell,
# the first-party launcher the desktop knows as /bin/browser, the resources
# beside them, and the fonts - which are a property of the machine
# (tools/install-fonts.sh) but which a browser is the first thing to need.
#
# This replaced tools/install-netsurf.sh in M171. What it installs is what the
# [m113] boot self-test grades, and what the interactive suite's browser tests
# open by double-clicking the Browser icon.

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC="$ROOT/build/chromium/src"
OUT_NAME="${LEANOS_CHROMIUM_OUT:-LeanOS}"
SHELL_BIN="$SRC/out/$OUT_NAME/content_shell"
SHELL_PAK="$SRC/out/$OUT_NAME/content_shell.pak"
IMAGE="${LEANOS_IMAGE:-build/os-image.bin}"
PUT="$ROOT/build/leanfs-put"
CROSS_STRIP="$ROOT/build/toolchain/bin/x86_64-lean_os-strip"

if [ ! -f "$SHELL_BIN" ]; then
  echo "install-browser: no $SHELL_BIN - tools/build-chromium.sh content/shell:content_shell builds it."
  echo "                 (An image without a browser is a valid image.)"
  exit 0
fi
if [ ! -f "$IMAGE" ]; then
  echo "install-browser: no $IMAGE - run make first" >&2
  exit 1
fi
[ -x "$PUT" ] || make -s leanfs-put || exit 1

# Stripped, for M165's reason: the kernel reads the whole file into its own
# heap before it copies the program into the process, so a symbol table would
# be paid for twice to answer questions only the host asks. 343 MB becomes
# about 220.
STRIPPED="$ROOT/build/content_shell.stripped"
if [ ! -f "$STRIPPED" ] || [ "$SHELL_BIN" -nt "$STRIPPED" ]; then
  cp "$SHELL_BIN" "$STRIPPED" || exit 1
  "$CROSS_STRIP" "$STRIPPED" || exit 1
fi
"$PUT" "$IMAGE" "$STRIPPED" /bin/chromiumshell >/dev/null || exit 1
before=$(wc -c < "$SHELL_BIN")
after=$(wc -c < "$STRIPPED")
echo "install-browser: /bin/chromiumshell ($((after / 1024 / 1024)) MB, was $((before / 1024 / 1024)) MB unstripped)"

# Beside the binary, because that is where DIR_ASSETS is and where
# ShellMainDelegate::InitializeResourceBundle looks for it.
"$PUT" "$IMAGE" "$SHELL_PAK" /bin/content_shell.pak >/dev/null || exit 1
echo "install-browser: /bin/content_shell.pak beside it"

# The launcher is a USER_PROGRAM, so `make preseed` has already put it in
# /bin/browser; this only says so, and refuses an image that lost it.
if [ ! -f "$ROOT/build/browser.elf" ]; then
  echo "install-browser: no build/browser.elf - run make" >&2
  exit 1
fi
"$PUT" "$IMAGE" "$ROOT/build/browser.elf" /bin/browser >/dev/null || exit 1
echo "install-browser: /bin/browser, which execs it with this desktop's switches"

"$PUT" "$IMAGE" "$ROOT/tools/browser/home.html" /usr/share/browser/home.html >/dev/null || exit 1
"$PUT" "$IMAGE" "$ROOT/tools/browser/flex.html" /usr/share/browser/flex.html >/dev/null || exit 1
echo "install-browser: /usr/share/browser/home.html and flex.html"

"$ROOT/tools/install-fonts.sh" || exit 1

CA_PKG=$(ls build/repo/ca-certificates-*.osp 2>/dev/null | head -1)
if [ -n "$CA_PKG" ] && [ -f build/repo/index ]; then
  make -s packages >/dev/null || exit 1
  LIST=$(mktemp -t leanos-preinstall-XXXXXX)
  printf 'ca-certificates\n' > "$LIST"
  "$PUT" "$IMAGE" "$LIST" /pkg/repo/preinstall >/dev/null || { rm -f "$LIST"; exit 1; }
  rm -f "$LIST"
  echo "install-browser: ca-certificates in /pkg/repo, installed by \`os preinstall\` on first boot"
else
  echo "install-browser: no ca-certificates package in build/repo - https will fail"
  echo "                 certificate checks. tools/build-packages.sh ca-certificates builds it."
fi

echo "install-browser: done - the Browser icon runs /bin/browser"
