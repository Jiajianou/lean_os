#!/usr/bin/env bash
set -uo pipefail

# M227. The instructions V8's embedded builtins use, held to the baseline the
# rest of the program was compiled for.
#
#   tools/v8-builtins-isa.sh <binary>
#
# mksnapshot compiles every builtin at BUILD time, on the build host, and takes
# the CPU features it may use from the macros of the compiler that built
# mksnapshot - not from the target's flags. On an Intel Mac that compiler
# targets penryn, so the first Intel-Mac node carried pextrd, pinsrd and
# roundsd (SSE4.1) in Builtins_MathMin and friends while its own code was
# -msse3, and died on its first statement on QEMU's default CPU. Patch 0062 is
# the fix; this is what says the fix holds, without depending on which CPU a
# test machine happens to emulate.
#
# The builtins are the Builtins_* symbols, which the embedded blob lays out as
# one contiguous run. Anything past SSE3 in that run is a failure: SSSE3,
# SSE4.1/4.2, AVX in any form (every VEX instruction is spelled v...), and the
# bit-manipulation extensions. The JIT is not graded here - it probes the CPU
# it runs on, which is the point of a JIT.

cd "$(dirname "$0")/.."
ROOT=$(pwd)
PREFIX="$ROOT/build/toolchain/bin/x86_64-lean_os-"
BINARY="$1"

RANGE=$("${PREFIX}nm" -S --defined-only "$BINARY" |
  awk '$4 ~ /^Builtins_/ { print $1, $2 }' | sort |
  awk 'NR == 1 { start = $1 } { end = $1; size = $2 }
       END { if (start != "") print start, end, size }')
if [ -z "$RANGE" ]; then
  echo "v8-builtins-isa: no Builtins_ symbols in $BINARY" >&2
  exit 2
fi
read -r START LAST SIZE <<< "$RANGE"
STOP=$(printf '0x%x' $((0x$LAST + 0x$SIZE)))

BEYOND='^(v[a-z]|pshufb|palignr|pabs|phadd|phsub|pmaddubsw|pmulhrsw|psign|pextr[bdq]|pinsr[bdq]|ptest|round[sp][sd]|pblend|blendv|blendp|pmax[su][bdw]|pmin[su][bdw]|insertps|extractps|dpp[sd]|pcmp[eg][qt]q|pmov[sz]x|packusdw|pmuldq|pmulld|mpsadbw|phminposuw|movntdqa|pcmp[ei]str|crc32|popcnt|lzcnt|tzcnt|andn|bextr|bls[ir]|blsmsk|shlx|shrx|sarx|rorx|pdep|pext|mulx|movbe)'
FOUND=$("${PREFIX}objdump" -d --no-show-raw-insn \
          --start-address="0x$START" --stop-address="$STOP" "$BINARY" |
  awk -F'\t' 'NF >= 2 { split($2, word, " "); print word[1] }' |
  grep -E "$BEYOND" | sort | uniq -c | sort -rn)

if [ -n "$FOUND" ]; then
  echo "v8-builtins-isa: instructions past SSE3 in the embedded builtins of $BINARY:" >&2
  echo "$FOUND" | sed 's/^/  /' >&2
  exit 1
fi
exit 0
