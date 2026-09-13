#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f build/os-image.bin ]; then
  echo "No image at build/os-image.bin yet - run 'make' first." >&2
  exit 1
fi

exec python3 tools/qemu_input_suite.py "$@"
