#!/bin/sh
# tools/gen-kernel-syms.sh - M101
#
# Turns the linked kernel's symbol table into the file /bin/profile
# resolves sampled addresses against.
#
# Usage: gen-kernel-syms.sh <kernel.elf> <out>
#
# ---- Why this is a file and not a table inside the kernel -------------
#
# A symbol table compiled into the kernel changes the addresses it
# describes, so it takes two link passes and a fixed-point iteration to
# converge (this is what Linux's kallsyms does, and it is a real amount
# of build machinery). Nothing here needs that: the profiler stores raw
# addresses, and resolution happens in user space, where a wrong answer
# is a wrong line of output rather than a fault in an interrupt handler.
#
# ---- The format -------------------------------------------------------
#
#   <hex address> <name>
#
# One per line, ascending by address, text symbols only. Ascending
# because the resolver binary-searches it, and "the symbol this address
# belongs to" is the last entry not greater than the address - which
# needs order and nothing else. No sizes: a text symbol's range is the
# next symbol's address, which is true for every entry but the last and
# is the reason the last line of this file is the end-of-text marker
# below rather than a real function.
#
# Local symbols ('t') are kept alongside global ones ('T'). Most of this
# kernel is static, so dropping them would resolve half the machine's
# code to whichever exported function happened to precede it - which is
# worse than an unresolved address, because it looks like an answer.
set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 <kernel.elf> <out>" >&2
    exit 2
fi

elf=$1
out=$2
nm=${NM:-x86_64-elf-nm}

if ! command -v "$nm" >/dev/null 2>&1; then
    echo "gen-kernel-syms: $nm not found (set NM=)" >&2
    exit 1
fi

# -n sorts numerically by address. The awk keeps text symbols and drops
# nm's own "no address" rows (undefined symbols print a blank column).
#
# The `.` filter drops assembler-local labels like `_start.zero_bss`:
# they are real addresses inside a function that already has a name, and
# resolving a sample to `_start.hang` rather than `_start` is noise in a
# report whose whole job is to be read by a person.
"$nm" -n "$elf" \
  | awk '($2 == "t" || $2 == "T") && $1 != "" && $3 !~ /\./ { print $1, $3 }' \
  > "$out.tmp"

if [ ! -s "$out.tmp" ]; then
    echo "gen-kernel-syms: $elf yielded no text symbols" >&2
    rm -f "$out.tmp"
    exit 1
fi

# The end marker. Without it the last real function extends to infinity,
# so every address past the end of .text - a wild jump, a corrupted
# return - resolves to whatever happens to be last in the file and reads
# like a plausible hotspot. An explicit end means the resolver can say
# "outside the kernel" and be believed.
# From the section header rather than from a linker-script symbol:
# kernel/linker.ld's __kernel_end is the end of BSS, which is hundreds of
# kilobytes past the end of code, and using it would make the last
# function in the file appear to span the whole data segment. .text's
# address plus its size is the actual boundary.
# The arithmetic is the shell's, not awk's: `strtonum` is a gawk
# extension and the awk on a Mac is the one-true-awk, which does not have
# it. This script runs on whatever is in front of the developer.
objdump=${OBJDUMP:-x86_64-elf-objdump}
text=$("$objdump" -h "$elf" | awk '$2 == ".text" { print $3, $4 }')
end=""
if [ -n "$text" ]; then
    size=$(echo "$text" | cut -d' ' -f1)
    addr=$(echo "$text" | cut -d' ' -f2)
    end=$(printf '%016x' $(( 0x$addr + 0x$size )))
fi
if [ -z "$end" ]; then
    # No .text header to read: the last symbol plus a page. Overstates
    # the final function by at most that, and is still bounded - unlike
    # leaving it to run to infinity, where every wild address resolves to
    # whatever happens to be last and reads like a plausible hotspot.
    last=$(tail -1 "$out.tmp" | cut -d' ' -f1)
    end=$(printf '%016x' $(( 0x$last + 0x1000 )))
fi
echo "$end [end-of-text]" >> "$out.tmp"

mv "$out.tmp" "$out"
echo "gen-kernel-syms: $(wc -l < "$out" | tr -d ' ') symbols -> $out"
