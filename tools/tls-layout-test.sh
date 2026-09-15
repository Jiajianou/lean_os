#!/bin/sh
# M147: where a thread_local really is, decided by the linker rather than by
# this project.
#
# __lean_tls_setup has to put the thread-local block exactly where the
# compiler will look for it. The compiler reads a variable at
#
#     tp - ALIGN(segment size, segment alignment) + its offset in the segment
#
# and the runtime used to subtract the segment SIZE, unrounded. Those are the
# same number whenever the size is already a multiple of the alignment, which
# it was in every program this project had ever linked - so the bug survived
# from M79 to M147 and first appeared as Chromium's //base jumping through
# half of a vtable pointer.
#
# This grades the formula against the only authority on it: each probe carries
# a .tlsprobe section of `symbol@tpoff` words, which the LINKER fills in with
# the offset the program's own code will use. The test requires the runtime's
# arithmetic to reproduce every one of them, over shapes chosen so that the
# rounding matters - and refuses to pass if none of the shapes is skewed,
# because a suite of aligned segments grades nothing.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

PREFIX="$ROOT/build/toolchain"
CC="$PREFIX/bin/x86_64-lean_os-gcc"
CXX="$PREFIX/bin/x86_64-lean_os-clang"
READELF="$PREFIX/bin/x86_64-lean_os-readelf"
OBJCOPY="$PREFIX/bin/x86_64-lean_os-objcopy"

if [ ! -x "$CC" ]; then
  echo "tls-layout-test: no $CC - run tools/build-toolchain.sh (dev-time only)"
  exit 0
fi

OUT="$ROOT/build/tls-layout"
rm -rf "$OUT"
mkdir -p "$OUT"

FAILED=0
CHECKS=0
SKEWED=0
ALIGNMENTS=""

fail() {
  echo "tls-layout-test: $*" >&2
  FAILED=$((FAILED + 1))
}

# Everything below is the runtime's own arithmetic, written once here and
# compared against the linker. Keep it in step with __lean_tls_setup.
round_up() {
  # round_up <value> <alignment>
  echo $(( ($1 + $2 - 1) / $2 * $2 ))
}

probe_one() {
  compiler="$1"; name="$2"; wide="$3"; middle="$4"; tail="$5"
  src="$OUT/$name.c"
  elf="$OUT/$name.elf"
  sed -e "s/WIDE_TYPE/$wide/" -e "s/MIDDLE_SIZE/$middle/" \
      -e "s/TAIL_SIZE/$tail/" tests/tls/probe.c.in > "$src"
  if ! "$compiler" -O1 "$src" -o "$elf" 2>"$OUT/$name.log"; then
    fail "$name did not build with $(basename "$compiler")"
    sed 's/^/    /' "$OUT/$name.log" >&2
    return
  fi

  set -- $("$READELF" -lW "$elf" | awk '$1=="TLS"{print $6, $NF; exit}')
  memsz=$([ -n "${1:-}" ] && echo $(( ${1} )))
  align=$([ -n "${2:-}" ] && echo $(( ${2} )))
  if [ -z "$memsz" ] || [ -z "$align" ]; then
    fail "$name has no PT_TLS segment - the probe did not produce thread-locals"
    return
  fi

  # The linker script's export has to BE the segment alignment. Nothing else
  # in the program carries that number, and the runtime cannot compute it.
  exported=$("$READELF" -sW "$elf" | awk '$8=="__lean_tls_align"{print "0x" $2; exit}')
  exported=$([ -n "$exported" ] && echo $(( exported )))
  if [ -z "$exported" ]; then
    fail "$name has no __lean_tls_align - user.ld did not export it"
    return
  fi
  CHECKS=$((CHECKS + 1))
  if [ "$exported" != "$align" ]; then
    fail "$name: user.ld says the alignment is $exported, the segment says $align"
  fi

  span=$(round_up "$memsz" "$align")
  if [ "$span" != "$memsz" ]; then
    SKEWED=$((SKEWED + 1))
  fi
  case " $ALIGNMENTS " in
    *" $align "*) ;;
    *) ALIGNMENTS="$ALIGNMENTS $align" ;;
  esac

  # The oracle: three 64-bit words the linker resolved, in declaration order.
  "$OBJCOPY" -O binary --only-section=.tlsprobe "$elf" "$OUT/$name.bin" 2>/dev/null
  if [ ! -s "$OUT/$name.bin" ]; then
    fail "$name has no .tlsprobe section to read"
    return
  fi

  i=0
  for symbol in probe_wide probe_middle probe_tail; do
    offset=$("$READELF" -sW "$elf" | awk -v s="$symbol" '$8==s{print "0x" $2; exit}')
    offset=$([ -n "$offset" ] && echo $(( offset )))
    if [ -z "$offset" ]; then
      fail "$name: $symbol is not in the symbol table"
      i=$((i + 1))
      continue
    fi
    actual=$(od -An -td8 -j $((i * 8)) -N 8 "$OUT/$name.bin" | tr -d ' \n')
    expected=$((offset - span))
    CHECKS=$((CHECKS + 1))
    if [ "$actual" != "$expected" ]; then
      fail "$name: the linker puts $symbol at tp$actual, this formula says tp$expected (size $memsz, alignment $align)"
    fi
    i=$((i + 1))
  done

  printf '  %-26s size=%-5s alignment=%-3s span=%-5s %s\n' \
    "$name" "$memsz" "$align" "$span" \
    "$([ "$span" != "$memsz" ] && echo "skewed by $((span - memsz))" || echo "already aligned")"
}

echo "tls-layout-test: the linker's own tpoff against __lean_tls_setup's arithmetic"

probe_one "$CC" gcc-align8-aligned  "unsigned long long" 5 3
probe_one "$CC" gcc-align8-skew4    "unsigned long long" 5 7
probe_one "$CC" gcc-align8-skew1    "unsigned long long" 1 1
probe_one "$CC" gcc-align4-small    "unsigned int"       3 1
probe_one "$CC" gcc-align16-skew    "long double"        5 3

if [ -x "$CXX" ]; then
  probe_one "$CXX" clang-align8-skew4 "unsigned long long" 5 7
  probe_one "$CXX" clang-align16-skew "long double"        5 3
else
  echo "  (no $CXX - skipping the clang half; run tools/build-clang.sh)"
fi

# And the invariant over every program already linked in this tree, which is
# the half that would notice user.ld's export going quietly wrong.
SWEPT=0
for elf in build/*.elf; do
  [ -f "$elf" ] || continue
  set -- $("$READELF" -lW "$elf" 2>/dev/null | awk '$1=="TLS"{print $6, $NF; exit}')
  [ -n "${2:-}" ] || continue
  segment_align=$(( $2 ))
  exported=$("$READELF" -sW "$elf" 2>/dev/null | awk '$8=="__lean_tls_align"{print "0x" $2; exit}')
  [ -n "$exported" ] || continue
  exported=$(( exported ))
  SWEPT=$((SWEPT + 1))
  CHECKS=$((CHECKS + 1))
  if [ "$exported" != "$segment_align" ]; then
    fail "$(basename "$elf"): user.ld says $exported, the segment says $segment_align"
  fi
done
echo "  and $SWEPT linked programs in build/ agree about their own alignment"

# The guards against a suite that cannot fail.
if [ "$SKEWED" -eq 0 ]; then
  fail "no probe produced a segment whose size needs rounding - this suite would pass with the bug in place"
fi
count=0
for a in $ALIGNMENTS; do count=$((count + 1)); done
if [ "$count" -lt 2 ]; then
  fail "every probe had the same alignment ($ALIGNMENTS) - the rounding is not being exercised"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "tls-layout-test: $FAILED failed out of $CHECKS" >&2
  exit 1
fi
echo "tls-layout-test: $CHECKS checks, $SKEWED skewed segments, alignments$ALIGNMENTS - all agree"
exit 0
