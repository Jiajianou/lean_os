#!/usr/bin/env bash
set -uo pipefail

# Copy a tree over another, touching only the files whose CONTENTS differ.
#
# `cp -R` rewrites the modification time of every file it copies, and for the
# sysroot that is not a detail: Chromium's build watches those headers, so
# refreshing them to add one function made siso rebuild libc++, ANGLE, Blink
# and everything else that includes a C header - an hour, every time, for a
# header nobody had changed. M169 measured that and this is the answer.
#
# Contents rather than timestamps, because the source of truth is a checked
# out tree whose mtimes mean nothing about whether the bytes moved.

source_directory="$1"
destination_directory="$2"

copied=0
total=0
while IFS= read -r relative; do
  total=$(( total + 1 ))
  source_file="$source_directory/$relative"
  destination_file="$destination_directory/$relative"
  if cmp -s "$source_file" "$destination_file"; then
    continue
  fi
  mkdir -p "$(dirname "$destination_file")"
  cp "$source_file" "$destination_file" || exit 1
  copied=$(( copied + 1 ))
done < <(cd "$source_directory" && find . -type f | sed 's|^\./||')

echo "$copied of $total"
