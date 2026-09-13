#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

HOSTCC="${HOSTCC:-cc}"
REFERENCE_SH="${REFERENCE_SH:-/bin/sh}"
FILTER="${1:-}"
BUILD=build
OURS="$BUILD/sh-host"

mkdir -p "$BUILD"

if [ ! -x "$REFERENCE_SH" ]; then
  echo "sh-test: no reference shell at $REFERENCE_SH - set REFERENCE_SH" >&2
  exit 1
fi

SHINC="$BUILD/sh-host-include"
mkdir -p "$SHINC"
cp system_api/include/paths.h "$SHINC/paths.h"

if ! $HOSTCC -std=c11 -O1 -Wall -Wextra -Werror \
     -I"$SHINC" -o "$OURS" user_space/shell/sh.c 2>"$BUILD/sh-host.log"; then
  echo "sh-test: the shell does not compile for the host:" >&2
  cat "$BUILD/sh-host.log" >&2
  exit 1
fi

pass=0
fail=0
failed_names=""

for fixture in tests/sh/*.sh; do
  name="$(basename "$fixture")"
  if [ -n "$FILTER" ] && [ "${name#*"$FILTER"}" = "$name" ]; then
    continue
  fi

  ours_dir="$(mktemp -d "${TMPDIR:-/tmp}/sh-ours.XXXXXX")"
  ref_dir="$(mktemp -d "${TMPDIR:-/tmp}/sh-ref.XXXXXX")"
  cp "$fixture" "$ours_dir/fixture.sh"
  cp "$fixture" "$ref_dir/fixture.sh"

  ( cd "$ours_dir" && "$OLDPWD/$OURS" fixture.sh ) > "$ours_dir/out" 2>&1
  ours_status=$?
  ( cd "$ref_dir" && "$REFERENCE_SH" fixture.sh ) > "$ref_dir/out" 2>&1
  ref_status=$?

  if cmp -s "$ours_dir/out" "$ref_dir/out" && [ "$ours_status" = "$ref_status" ]; then
    echo "  ok   $name"
    pass=$((pass + 1))
  else
    echo "  FAIL $name"
    if [ "$ours_status" != "$ref_status" ]; then
      echo "       exit status: ours $ours_status, $REFERENCE_SH $ref_status"
    fi
    diff -u "$ref_dir/out" "$ours_dir/out" | sed -n '1,40p' | sed 's/^/       /'
    fail=$((fail + 1))
    failed_names="$failed_names $name"
  fi
  rm -rf "$ours_dir" "$ref_dir"
done

echo
if [ "$fail" -eq 0 ]; then
  echo "PASS: $pass shell fixture(s) agree with $REFERENCE_SH byte for byte."
  exit 0
fi
echo "FAIL: $fail of $((pass + fail)) fixture(s) differ from $REFERENCE_SH:$failed_names"
exit 1
