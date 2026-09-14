#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

WORK="$ROOT/build/chromium"
DEPOT="$WORK/depot_tools"
SRC="$WORK/src"
VENDORED=third_party/chromium/VENDORED

REQUIRED_GIB=200

if [ ! -f "$VENDORED" ]; then
  echo "fetch-chromium: $VENDORED is missing" >&2
  exit 1
fi

REVISION=$(awk '/^REVISION /{print $2}' "$VENDORED")
if [ -z "$REVISION" ]; then
  echo "fetch-chromium: $VENDORED names no REVISION" >&2
  exit 1
fi

AVAIL_GIB=$(df -g "$ROOT" | awk 'NR==2{print $4}')
if [ "$AVAIL_GIB" -lt "$REQUIRED_GIB" ]; then
  echo "fetch-chromium: $AVAIL_GIB GiB free, $REQUIRED_GIB GiB needed" >&2
  exit 1
fi

mkdir -p "$WORK"

if [ ! -d "$DEPOT" ]; then
  echo "fetch-chromium: cloning depot_tools"
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git \
      "$DEPOT" || exit 1
fi

if [ ! -d "$DEPOT/.cipd_bin" ]; then
  echo "fetch-chromium: bootstrapping depot_tools"
  (cd "$DEPOT" && ./ensure_bootstrap) || exit 1
fi

PATH="$DEPOT:$DEPOT/.cipd_bin:$PATH"
export PATH
export DEPOT_TOOLS_UPDATE=0

if [ ! -d "$SRC" ]; then
  echo "fetch-chromium: fetching the checkout - this takes hours"
  (cd "$WORK" && fetch --no-history chromium) || exit 1
fi

HAVE=$(cd "$SRC" && git rev-parse HEAD 2>/dev/null)
if [ "$HAVE" != "$REVISION" ]; then
  echo "fetch-chromium: syncing to $REVISION"
  (cd "$SRC" && git fetch origin "$REVISION" && git checkout --detach "$REVISION") || exit 1
fi

echo "fetch-chromium: syncing dependencies"
(cd "$SRC" && gclient sync --no-history --with_branch_heads --reset) || exit 1

echo "fetch-chromium: $SRC at $REVISION"
