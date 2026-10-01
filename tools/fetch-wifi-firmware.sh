#!/usr/bin/env bash
# Intel's firmware for the AX201 (22000 family, "Qu"/"QuZ" MAC with the "HR"
# radio), fetched from linux-firmware into build/firmware and checked against
# the hashes in third_party/intel_wireless_firmware/VENDORED. The files are
# redistributable unmodified (see the LICENCE beside VENDORED) and are never
# edited here; tools/make-hardware-image.sh puts them in /lib/firmware.
set -uo pipefail
cd "$(dirname "$0")/.."
VENDORED=third_party/intel_wireless_firmware/VENDORED
OUT=build/firmware
mkdir -p "$OUT"
SOURCE=$(awk '/^SOURCE /{print $2}' "$VENDORED")

check() {
  local path=$1 want=$2
  [ -f "$path" ] && [ "$(shasum -a 256 "$path" | awk '{print $1}')" = "$want" ]
}

status=0
while read -r kind remote hash; do
  [ "$kind" = FILE ] || [ "$kind" = LICENCE ] || continue
  local_path="$OUT/$(basename "$remote")"
  if check "$local_path" "$hash"; then
    continue
  fi
  echo "fetch-wifi-firmware: $(basename "$remote")"
  if ! curl -sfL -o "$local_path.part" "$SOURCE/$remote"; then
    echo "fetch-wifi-firmware: could not download $remote" >&2
    status=1
    continue
  fi
  mv "$local_path.part" "$local_path"
  if ! check "$local_path" "$hash"; then
    echo "fetch-wifi-firmware: $(basename "$remote") does not have the hash VENDORED pins" >&2
    rm -f "$local_path"
    status=1
  fi
done < "$VENDORED"
[ $status -eq 0 ] && echo "fetch-wifi-firmware: $OUT is current"
exit $status
