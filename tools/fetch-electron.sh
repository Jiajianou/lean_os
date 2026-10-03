#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

SRC="$ROOT/build/chromium/src"
VENDORED=third_party/electron/VENDORED

if [ ! -d "$SRC" ]; then
  echo "fetch-electron: no Chromium checkout - run tools/fetch-chromium.sh" >&2
  exit 1
fi

pin() {
  awk -v key="$1" '$1 == key {print $2}' "$VENDORED"
}

# Each of these is fetched at exactly one commit and nothing else: no gclient,
# because Electron's DEPS would sync src to 155.0.8059.0 and take the browser's
# checkout with it.
fetch_at() {
  local directory="$1" url="$2" commit="$3"
  if [ "$(git -C "$directory" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
    echo "fetch-electron: $directory at $commit"
    return 0
  fi
  mkdir -p "$directory"
  [ -d "$directory/.git" ] || git -C "$directory" init -q || return 1
  git -C "$directory" fetch -q --depth 1 "$url" "$commit" || return 1
  git -C "$directory" checkout -q --detach FETCH_HEAD || return 1
  echo "fetch-electron: $directory now at $commit"
}

fetch_at "$SRC/electron" https://github.com/electron/electron.git \
  "$(pin ELECTRON)" || exit 1
fetch_at "$SRC/third_party/electron_node" https://github.com/nodejs/node.git \
  "$(awk '$1 == "NODE" {print $3}' "$VENDORED")" || exit 1
fetch_at "$SRC/third_party/nan" https://github.com/nodejs/nan.git \
  "$(pin NAN)" || exit 1

python3 "$ROOT/tools/electron-fit.py"
