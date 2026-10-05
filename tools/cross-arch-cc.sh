#!/usr/bin/env bash
# tools/cross-arch-cc.sh - the host compiler, twice: once exactly as asked, and
# once more for the OTHER macOS architecture into a directory of its own.
#
# Not run by hand. tools/cross-arch-test.sh puts this on PATH as `cc` while it
# runs the differential tests, so every host compile those scripts make is
# also made with -arch arm64 (on an Intel Mac) or -arch x86_64 (on Apple
# Silicon), and the result goes into a ledger the stage grades.
#
# The native compile is the one the calling script sees: its arguments, its
# output files, its exit status, untouched - so the script runs exactly as it
# would have. The second compile has its -o (and -MF) moved under
# $CROSS_ARCH_ROOT/out, and an object or archive it links is taken from there
# when the second pass made one, so a link of other-arch objects is a link of
# other-arch objects. Nothing it writes is outside that directory.
#
# Environment (set by cross-arch-test.sh):
#   CROSS_ARCH        the architecture of the second compile
#   CROSS_ARCH_ROOT   absolute directory the second compile writes under
#   CROSS_ARCH_LABEL  which script is calling, for the ledger
#   CROSS_ARCH_CC     the real compiler (an absolute path, so this cannot
#                     find itself on PATH)
#
# The ledger is one entry per line - ok|FAIL|NATIVE, a tab, the label, a tab,
# what was run - with a FAIL's detail on the indented lines after it. A
# script may append a FAIL of its own for a check it makes on what the second
# compile wrote (tools/math-test.sh's nm check of the other Mac's objects),
# because the stage grades this ledger and not the script's exit status.
set -u

real="${CROSS_ARCH_CC:-/usr/bin/cc}"
"$real" "$@"
status=$?
if [ -z "${CROSS_ARCH:-}" ] || [ -z "${CROSS_ARCH_ROOT:-}" ]; then
  exit "$status"
fi
# The ledger is a directory with a file per entry, not one file appended to:
# the scripts run side by side, an entry with its compiler errors is a few
# KiB, and an append that large is more than one write - two of them could
# interleave mid-line and be counted as neither. cross-arch-test.sh reads
# them all.
record() {
  mkdir -p "$CROSS_ARCH_ROOT/ledger.d"
  local f
  f=$(mktemp "$CROSS_ARCH_ROOT/ledger.d/entry.XXXXXX") || return
  printf '%s\n' "$1" > "$f"
}
if [ "$status" -ne 0 ]; then
  # Failed for THIS Mac already: the ordinary stage reports that, and there
  # is no second compile to grade. Written down so the stage can say which.
  record "$(printf 'NATIVE\t%s\t%s' "${CROSS_ARCH_LABEL:-?}" "$*")"
  exit "$status"
fi

# Only an invocation that names its output is repeated: one that writes to
# stdout (-E, --version, -print-*) or to a default name in the working
# directory would have the second pass overwrite the first one's answer.
names_output=0
for a in "$@"; do
  case "$a" in
    -o) names_output=1 ;;
    -E|-M|-MM|--version|-v|-print-*|-dumpversion|-dumpmachine) exit 0 ;;
  esac
done
[ "$names_output" -eq 1 ] || exit 0

here=$(pwd)
map() {
  local p="$1"
  case "$p" in
    "$here"/*) p="${p#"$here"/}" ;;
    /*) p="abs$p" ;;
  esac
  printf '%s/out/%s' "$CROSS_ARCH_ROOT" "$p"
}

# An output whose second compile failed is marked <output>.failed, so that a
# link of it says which input never compiled rather than failing on the
# native object it would otherwise have fallen back to - "found architecture
# x86_64, required arm64" and a page of undefined symbols, for an error that
# was one #error two entries above.
args=()
next=""
output=""
missing=()
for a in "$@"; do
  case "$next" in
    map)
      mapped=$(map "$a")
      mkdir -p "$(dirname "$mapped")"
      args+=("$mapped")
      [ "${args[${#args[@]}-2]}" = "-o" ] && output="$mapped"
      next=""
      continue
      ;;
    raw)
      args+=("$a")
      next=""
      continue
      ;;
  esac
  case "$a" in
    -o|-MF) args+=("$a"); next=map ;;
    -MT|-MQ) args+=("$a"); next=raw ;;
    *.o|*.a)
      mapped=$(map "$a")
      [ -f "$mapped.failed" ] && missing+=("$a")
      if [ -f "$mapped" ]; then args+=("$mapped"); else args+=("$a"); fi
      ;;
    *) args+=("$a") ;;
  esac
done

errors=$(mktemp "$CROSS_ARCH_ROOT/err.XXXXXX")
if [ ${#missing[@]} -gt 0 ]; then
  printf 'not attempted: %s did not compile for %s (above)\n' "${missing[*]}" "$CROSS_ARCH" > "$errors"
  ok=0
elif "$real" -arch "$CROSS_ARCH" "${args[@]}" > /dev/null 2> "$errors"; then
  ok=1
else
  ok=0
fi
if [ "$ok" -eq 1 ]; then
  entry=$(printf 'ok\t%s\t%s' "${CROSS_ARCH_LABEL:-?}" "$*")
  [ -n "$output" ] && rm -f "$output.failed"
else
  entry=$(printf 'FAIL\t%s\t%s\n' "${CROSS_ARCH_LABEL:-?}" "$*"; sed 's/^/    /' "$errors" | head -20)
  [ -n "$output" ] && : > "$output.failed"
fi
record "$entry"
rm -f "$errors"
exit "$status"
