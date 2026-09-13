#!/bin/sh
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

"$nm" -n "$elf" \
  | awk '($2 == "t" || $2 == "T") && $1 != "" && $3 !~ /\./ { print $1, $3 }' \
  > "$out.tmp"

if [ ! -s "$out.tmp" ]; then
    echo "gen-kernel-syms: $elf yielded no text symbols" >&2
    rm -f "$out.tmp"
    exit 1
fi

objdump=${OBJDUMP:-x86_64-elf-objdump}
text=$("$objdump" -h "$elf" | awk '$2 == ".text" { print $3, $4 }')
end=""
if [ -n "$text" ]; then
    size=$(echo "$text" | cut -d' ' -f1)
    addr=$(echo "$text" | cut -d' ' -f2)
    end=$(printf '%016x' $(( 0x$addr + 0x$size )))
fi
if [ -z "$end" ]; then
    last=$(tail -1 "$out.tmp" | cut -d' ' -f1)
    end=$(printf '%016x' $(( 0x$last + 0x1000 )))
fi
echo "$end [end-of-text]" >> "$out.tmp"

mv "$out.tmp" "$out"
echo "gen-kernel-syms: $(wc -l < "$out" | tr -d ' ') symbols -> $out"
