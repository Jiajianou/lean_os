#!/usr/bin/env bash
# M206: the Intel wireless driver off the machine. Two instruments, because
# the device cannot be run anywhere but the laptop:
#   - every command and notification structure this driver writes or reads,
#     field by field, against Linux's own iwlwifi API headers (dual GPL/BSD),
#     fetched at a pinned commit into build/reference - a wrong offset is not
#     an error the firmware reports, it is the radio doing something else;
#   - the firmware parser against the three files Intel ships.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
LINUX_COMMIT=551c722f40809618230001baccf219193e22fc5a
REFERENCE=build/reference/linux
IWL=$REFERENCE/drivers/net/wireless/intel/iwlwifi
WORK=build/intel-wireless-test
mkdir -p "$WORK"

if [ "$(git -C "$REFERENCE" rev-parse HEAD 2>/dev/null)" != "$LINUX_COMMIT" ]; then
  echo "intel-wireless-test: fetching Linux's iwlwifi headers at $LINUX_COMMIT"
  rm -rf "$REFERENCE"
  mkdir -p "$REFERENCE"
  git -C "$REFERENCE" init -q &&
    git -C "$REFERENCE" remote add origin https://github.com/torvalds/linux.git &&
    git -C "$REFERENCE" sparse-checkout set drivers/net/wireless/intel/iwlwifi &&
    git -C "$REFERENCE" fetch -q --depth 1 --filter=blob:none origin "$LINUX_COMMIT" &&
    git -C "$REFERENCE" checkout -q FETCH_HEAD || { echo "FAIL: could not fetch the reference headers"; exit 1; }
fi
tools/fetch-wifi-firmware.sh >/dev/null || { echo "FAIL: could not fetch the firmware"; exit 1; }

L=tools/intel-wireless-layout
python3 $L/generate.py "$WORK" || exit 1
cc -w -c -o "$WORK/linux_side.o" -I$L/shim -I$L -I$IWL -include $L/prelude.h "$WORK/linux_side.c" || exit 1
cc -Wall -Werror -c -o "$WORK/lean_side.o" -I$L -Ikernel "$WORK/lean_side.c" || exit 1
cc -I$L -o "$WORK/layout" $L/compare.c "$WORK/linux_side.o" "$WORK/lean_side.o" || exit 1
"$WORK/layout" || exit 1

cc -Wall -Werror -DINTEL_WIRELESS_DRAM_ENTRIES_CHECK=64 -Ikernel -o "$WORK/real_firmware" \
  tests/intel_wireless/real_firmware.c kernel/drivers/intel_wireless_firmware.c || exit 1
"$WORK/real_firmware" build/firmware
