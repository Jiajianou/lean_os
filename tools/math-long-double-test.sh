#!/usr/bin/env bash
# M142. The host half of grading this libm's long double family.
#
# There is no differential test here in the shape tools/math-test.sh has, and
# the reason is the host. On an arm64 Mac `long double` IS `double`, and a
# host libm cannot answer a question about a 64-bit mantissa it does not
# have. An x86_64 Mac's long double is the x87's 80 bits and its libm could,
# but a test that only grades on one of the two hosts it runs on is half a
# test. So the oracle is MPFR, reached through the cross compiler - GCC folds
# __builtin_<name>l() on constant arguments at compile time, with MPFR, at
# the TARGET's precision - on either host, and the answers travel into the
# image as .rodata.
#
# What this script grades, therefore, is the instrument rather than the
# library: that the table still regenerates byte for byte from the case file,
# and that every expected value in it really was folded. The library itself
# is graded on the machine, by /bin/mathltest, because a wrong answer at the
# sixty-fourth bit builds and links perfectly well.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

HEADER=user_space/libc/include/math.h
CASES=tests/math/long_double_cases.tsv
TABLE=user_space/binaries/math_long_double_table.c
OUT=build/math-long-double
CROSS="${CROSS:-x86_64-elf}"

mkdir -p "$OUT"

PASS=0
FAIL=0
check() {
  if [ "$1" = "0" ]; then
    echo "math-long-double-test: pass - $2"
    PASS=$((PASS + 1))
  else
    echo "math-long-double-test: FAIL - $2" >&2
    FAIL=$((FAIL + 1))
  fi
}

NAMES=$(grep -oE '^(long double|long long|long|int) [a-z0-9_]+l\(' "$HEADER" \
        | sed -E 's/^(long double|long long|long|int) //; s/\($//; s/\(//' \
        | sort -u)
if [ -z "$NAMES" ]; then
  echo "math-long-double-test: no long double declarations found in $HEADER" >&2
  echo "                       - the grep needs updating" >&2
  exit 2
fi

missing=0
for n in $NAMES; do
  if ! awk -F'\t' -v n="$n" '$1 == n {found=1} END {exit !found}' "$CASES"; then
    echo "math-long-double-test: $n is declared in $HEADER and has no row in" >&2
    echo "                       $CASES. Add one - a '-' row with a note is a" >&2
    echo "                       valid answer; a missing row is a function" >&2
    echo "                       nobody grades." >&2
    missing=1
  fi
done
[ "$missing" -eq 0 ]
check $? "every long double declaration in $HEADER has a row in $CASES"

stale=0
for n in $(awk -F'\t' '!/^#/ && NF > 1 {print $1}' "$CASES"); do
  echo "$NAMES" | grep -qx "$n" || {
    echo "math-long-double-test: $n has a row in $CASES and is not declared" >&2
    echo "                       in $HEADER." >&2
    stale=1
  }
done
[ "$stale" -eq 0 ]
check $? "every row in $CASES names a function this libc declares"

# The quiet NaN probe in /bin/mathltest ([m142q]) asks every function what it
# raises at a NaN, in the compiler's output that ships. It is a list written by
# hand, so this is what keeps it whole: every function <math.h> declares, in
# all three formats, has a probe whose ARGUMENTS hold a quiet NaN, and which
# always runs. nan, nanf and nanl take a string.
#
# A probe is one line, QUIET(f, (arguments)) or EXACT, LOUD or LOUD_EXACT -
# the function named once and its argument list as a separate field, and a
# quiet NaN written only as NAN_D, NAN_F or NAN_L. That structure is what
# makes this check mean something. It used to accept any call of the right
# name anywhere in grade_quiet_nan, so a function whose NaN probe was deleted
# still passed on the strength of a call in a value check or a premise.
# mathltest.c counts NAN_* reads at run time as well, so an argument list
# that names no NaN fails on the machine even where this cannot see it.
#
# A probe that never runs reads no NaN and grades nothing, so neither that
# count nor a grep for the line would notice it - in a comment, under #if 0,
# or under an if. So the file is read the way the compiler reads it, comments
# blanked first (string and character literals kept, and a /* inside one is
# not a comment), and a line counts as a probe only as a statement of its
# own at grade_quiet_nan's top level: brace depth one, after a line that
# ends a statement or a block (so not the body of an if, else, for or while
# on the line above). grade_quiet_nan may hold no preprocessor line at all,
# and nothing that leaves it early (return, goto, longjmp, exit, abort).
ALL_NAMES=$(grep -oE '^(long double|double|float|long long|long|int) [a-z0-9_]+\(' "$HEADER" \
            | sed -E 's/^(long double|long long|double|float|long|int) //; s/\($//' \
            | sort -u)
# (Brackets rather than backslashes for a literal parenthesis: awk -v reads
# backslash escapes in what it is given, and would take \( for a group.)
PROBE_LINE='^[[:space:]]*(QUIET|EXACT|LOUD|LOUD_EXACT)[(]([a-z0-9_]+), [(]([^()]*)[)]'

# quiet_nan_body FILE: grade_quiet_nan's lines, comments blanked, each as
# "probe<TAB>line" for a probe that always runs, "code<TAB>line" otherwise,
# or "bad<TAB>line number<TAB>why" for what this check refuses.
quiet_nan_body() {
  awk -v probe="$PROBE_LINE" '
    BEGIN { state = 0; inside = 0 }   # state: 0 code, 1 /* */, 2 "", 3 '"''"'
    {
      line = $0; code = ""; n = length(line)
      for (i = 1; i <= n; i++) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (state == 1) {
          if (d == "*/") { state = 0; i++; code = code " " }
          continue
        }
        if (state >= 2) {
          code = code c
          if (c == "\\") { code = code substr(line, i + 1, 1); i++ }
          else if ((state == 2 && c == "\"") || (state == 3 && c == "'"'"'")) state = 0
          continue
        }
        if (d == "/*") { state = 1; i++; continue }
        if (d == "//") break
        if (c == "\"") state = 2
        else if (c == "'"'"'") state = 3
        code = code c
      }
      if (!inside) {
        if (code ~ /^static void grade_quiet_nan\(void\) \{/) {
          inside = 1; depth = 1; previous = "{"; found = 1
        }
        next
      }
      bare = code
      gsub(/"([^"\\]|\\.)*"/, "\"\"", bare)
      gsub(/'"'"'([^'"'"'\\]|\\.)*'"'"'/, "'"''"'", bare)
      before = depth
      opens = gsub(/\{/, "{", bare); closes = gsub(/\}/, "}", bare)
      depth += opens - closes
      if (depth <= 0) exit
      if (bare ~ /^[ \t]*#/) {
        print "bad\t" NR "\ta preprocessor line - a probe under it could be compiled out"
      }
      if (bare ~ /(^|[^A-Za-z0-9_])(return|goto|longjmp|siglongjmp|exit|_Exit|abort)([^A-Za-z0-9_]|$)/) {
        print "bad\t" NR "\tsomething that leaves before the probes after it run"
      }
      if (before == 1 && code ~ probe && previous ~ /[;{}]$/) {
        print "probe\t" code
      } else {
        print "code\t" code
      }
      trimmed = bare
      sub(/[ \t]+$/, "", trimmed)
      if (trimmed !~ /^[ \t]*$/) previous = trimmed
    }
    END { if (!found) print "bad\t0\tno grade_quiet_nan" }
  ' "$1"
}

# probe_coverage FILE: names every declared function that no probe in FILE's
# grade_quiet_nan hands a quiet NaN, and fails if there is one.
probe_coverage() {
  local body probed n missing=0
  body=$(quiet_nan_body "$1")
  if printf '%s\n' "$body" | grep -q '^bad	'; then
    printf '%s\n' "$body" | awk -F'\t' -v f="$1" '$1 == "bad" {
      print "math-long-double-test: " f ":" $2 ": grade_quiet_nan holds " $3 > "/dev/stderr" }'
    missing=1
  fi
  probed=$(printf '%s\n' "$body" | sed -n 's/^probe	//p' | grep -E "$PROBE_LINE" \
           | sed -E "s/$PROBE_LINE.*/\2 \3/" \
           | awk '$0 ~ /NAN_[DFL]/ {print $1}' | sort -u)
  for n in $ALL_NAMES; do
    case "$n" in nan|nanf|nanl) continue ;; esac
    if ! printf '%s\n' "$probed" | grep -qx "$n"; then
      echo "math-long-double-test: $n is declared in $HEADER and no probe in" >&2
      echo "                       $1 that always runs hands it a quiet NaN." >&2
      missing=1
    fi
  done
  return "$missing"
}

probe_coverage user_space/binaries/mathltest.c
check $? "every function $HEADER declares has a quiet NaN probe in /bin/mathltest"

# And the check is shown to fail, every run, rather than trusted to - on one
# function's probe taken away in each of the ways it can go: deleted; still
# there with its NaN replaced by a number; and still there but never run,
# inside a /* */ block, under #if 0, and as the body of an if. The copies are build
# output. fabs is the function the first check could not lose: copysign's
# probe calls fabs(r) in its value check, which satisfied it with fabs's own
# probe gone.
VICTIM=fabs
VICTIM_LINE="^[[:space:]]*(QUIET|EXACT|LOUD|LOUD_EXACT)[(]$VICTIM, "
SOURCE=user_space/binaries/mathltest.c
# mutate HOW: the copy, checked to differ from the source by exactly the
# lines HOW adds or takes away - so that a copy the edit garbled cannot fail
# the check for a reason that is not the one being shown.
mutate() {
  local lines
  awk -v victim="$VICTIM_LINE" -v how="$1" '
    $0 ~ victim {
      if (how == "removed") next
      if (how == "without-nan") { gsub(/NAN_D/, "one_d") }
      if (how == "commented") { print "    /*"; print; print "    */"; next }
      if (how == "if-0") { print "#if 0"; print; print "#endif"; next }
      if (how == "under-if") { print "    if (zero_d != 0.0)"; print; next }
    }
    { print }' "$SOURCE" > "$OUT/mathltest-probe-$1.c" || return 1
  lines=$(( $(wc -l < "$OUT/mathltest-probe-$1.c") - $(wc -l < "$SOURCE") ))
  [ "$lines" -eq "$2" ] && ! cmp -s "$SOURCE" "$OUT/mathltest-probe-$1.c"
}
caught=0
for change in removed:-1 without-nan:0 commented:2 if-0:2 under-if:1; do
  how=${change%:*}
  if ! mutate "$how" "${change#*:}"; then
    echo "math-long-double-test: could not make the copy with $VICTIM's probe $how" >&2
    echo "                       - mathltest.c has no one $VICTIM probe line to change" >&2
    caught=1
  elif probe_coverage "$OUT/mathltest-probe-$how.c" 2> /dev/null; then
    echo "math-long-double-test: the probe check passed with $VICTIM's probe $how" >&2
    caught=1
  fi
done
[ "$caught" -eq 0 ]
check $? "that check fails with $VICTIM's probe removed, its NaN replaced by a number, or put where it never runs (a comment, #if 0, an if)"

python3 tools/gen-math-long-double-table.py "$OUT/regenerated.c" || exit 1
cmp -s "$OUT/regenerated.c" "$TABLE"
check $? "$TABLE regenerates byte for byte from $CASES"

if ! command -v "$CROSS-gcc" > /dev/null 2>&1; then
  echo "math-long-double-test: no $CROSS-gcc - cannot check the fold"
else
  "$CROSS-gcc" -std=c11 -O1 -ffreestanding -fno-stack-protector -fno-pic \
      -mcmodel=large -mno-red-zone -Wall -Wextra -Werror \
      -I user_space/libc/include -I user_space/binaries \
      -c "$TABLE" -o "$OUT/table.o" 2> "$OUT/table.log"
  check $? "the reference table compiles, which is MPFR evaluating it"

  if [ -f "$OUT/table.o" ]; then
    # This is the whole instrument in one line. A __builtin_ call GCC could
    # not fold becomes a CALL to this libm, and this libm is what is being
    # graded - so an object with an undefined symbol in it is an exam that
    # marked itself.
    UNDEFINED=$("$CROSS-nm" -u "$OUT/table.o" | wc -l | tr -d ' ')
    [ "$UNDEFINED" = "0" ]
    check $? "the table references no symbol at all - every value was folded"

    VALUES=$(grep -c '__builtin_' "$TABLE")
    echo "math-long-double-test: $VALUES expected values, all of them MPFR's,"
    echo "                       at this target's own 64-bit mantissa."
  fi
fi

# The grader goes onto the image the way /bin/clangtest and /bin/ruststd do,
# and for the same reason: the kernel incbins every embedded program, and
# 12,840 answers is more data than a kernel should carry. $(IMAGE)'s recipe
# recreates the disk whenever the kernel is newer, so this runs after a build
# rather than as part of one.
IMAGE=build/os-image.bin
if [ "${LEANOS_NO_INSTALL:-}" = "1" ]; then
  echo "math-long-double-test: not installing (LEANOS_NO_INSTALL=1)"
elif [ ! -f "$IMAGE" ]; then
  echo "math-long-double-test: no $IMAGE yet - run make, then this again"
else
  make build/mathltest.elf > "$OUT/build.log" 2>&1 || {
    tail -20 "$OUT/build.log" >&2; exit 1; }
  make leanfs-put > /dev/null 2>&1 || exit 1
  build/leanfs-put "$IMAGE" build/mathltest.elf /bin/mathltest > /dev/null
  check $? "installed as /bin/mathltest - the [m142] boot self-test runs it"
fi

GRADED=$(awk -F'\t' '!/^#/ && NF > 1 && $2 != "-"' "$CASES" | wc -l | tr -d ' ')
DEFERRED=$(awk -F'\t' '!/^#/ && NF > 1 && $2 == "-"' "$CASES" | wc -l | tr -d ' ')
echo "math-long-double-test: $GRADED functions swept and $DEFERRED graded by"
echo "                       an identity on the machine; /bin/mathltest is"
echo "                       what runs the comparison, in the boot battery."

if [ "$FAIL" -ne 0 ]; then
  echo
  echo "FAIL - $FAIL of $((PASS + FAIL)) checks"
  exit 1
fi
echo "math-long-double-test: $PASS checks passed"
