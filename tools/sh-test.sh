#!/usr/bin/env bash
# tools/sh-test.sh - M86: grade this project's shell against a real one.
#
# Compiles user_space/shell/sh.c FOR THE HOST - the same source the
# machine runs - and runs every fixture in tests/sh/ through it and
# through the host's own /bin/sh. The two outputs must be byte-identical.
#
# ---- why this is possible, and why that matters ----------------------
#
# M72's shell was written against this project's `sys_` wrappers and
# could only run on lean_os, so the only way to grade it was to boot a
# machine and grep a serial log for strings this project had chosen
# itself. That is a test of "did it do what I expected", and for a shell
# it is close to worthless: the entire job of a shell is to agree with
# every other shell about what a script means.
#
# M86's shell calls open, read, write, dup2, pipe, fork, execve, waitpid
# and opendir and nothing else, because M75-M85 made those exist here.
# So it compiles for the host, and the host has a reference
# implementation of the thing being tested sitting in /bin/sh. Nothing in
# tests/sh says what the right answer is. The reference decides.
#
# What this CANNOT see is anything about lean_os: its fork, its pipes,
# its `#!` handling, its spawn path. kernel.c's M86 self-test covers
# that, and neither instrument subsumes the other.
#
# Usage:
#   tools/sh-test.sh              # every fixture
#   tools/sh-test.sh 05           # just the fixtures matching "05"
#   REFERENCE_SH=/bin/dash tools/sh-test.sh
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

# -DSH_HOST_BUILD is not needed and deliberately not used: the point is
# that this is the same translation unit the machine gets, with no
# conditional compilation deciding which shell is being tested.
# ---- M99: why the include path is one file rather than one directory ---
#
# It was -Isystem_api/include, which is there for exactly one header -
# paths.h, where PATH_DEFAULT and PATH_HOME are written down. That
# directory also holds this project's own <signal.h>, which is the
# KERNEL/USER ABI header and defines `siginfo_t` as the machine's own
# struct. Ahead of the host's headers on the search path, it shadows the
# host's <signal.h>, and the first thing that noticed was the compile of
# a shell that had just learned `trap`:
#
#   error: typedef redefinition with different types
#          ('struct __siginfo' vs 'struct siginfo_t')
#
# So the directory becomes the one file it was ever for. sh.c is
# unchanged and still has no idea which machine it is being built for,
# which is the property this whole script exists to preserve.
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

  # Each run gets its own directory, because fixtures create files and a
  # fixture that saw the previous one's output would pass for the wrong
  # reason - and would pass differently on a second run, which is worse.
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
