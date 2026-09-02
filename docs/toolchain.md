# Toolchain

Dev-time tools only — nothing here ships inside the OS image (see the
ground rules in [milestones.md](../milestones.md)).

| Tool | Version | Purpose | Installed via |
|---|---|---|---|
| `x86_64-elf-gcc` | 16.2.0 | freestanding C compiler | `brew install x86_64-elf-gcc` |
| `x86_64-elf-ld` | binutils 2.47 | linker, uses our own linker scripts | `brew install x86_64-elf-binutils` |
| `nasm` | 3.02 | assembler for boot code and other `.asm` | `brew install nasm` |
| `qemu-system-x86_64` | 10.1.3 | emulator for fast boot testing | already present |
| `clang` + `lld-link` | any recent | compiles/links `kernel/boot/uefi`'s `BOOTX64.EFI` (M24) | `brew install lld` (clang: Apple's own is enough) |
| `mtools` | any recent | formats the UEFI boot path's FAT ESP directly inside the disk image | `brew install mtools` |
| `llvm` (full keg) + `acpica` | any recent | only for `tools/build-ovmf.sh` (builds OVMF firmware from source) | `brew install llvm acpica` |
| `gsed` (GNU sed) | 4.10 | only for `tools/build-toybox.sh` (M89) — toybox's own build scripts need GNU sed | `brew install gnu-sed` |

## M89: GNU sed, and why a host tool for somebody else's build is listed here

`gsed` is the first entry in that table that this project's own build
never invokes. It is there because `tools/build-toybox.sh` drives
**toybox's** build system, and toybox's code-generation scripts are
written against GNU sed — on a Mac they fail at the first step, with a
`sed: 1: "...": invalid command code T` that says nothing useful about
the cause. Toybox looks for GNU sed under the name `gsed` specifically
(`scripts/portability.sh`), which is what Homebrew installs it as.

It ends up in the image no more than `nasm` or `qemu` does. Porting
somebody else's software means accepting their build's requirements, and
that is the first one.

**The second one is not a package, deliberately.** The same build pipes
`gzip` output through `od -Anone -vtx1` into a `sed` that turns each
space into `,0x`. GNU `od` emits exactly one space before each byte; BSD
`od`, which is what macOS has, indents the line and pads it out with
trailing spaces — so the same `sed` produces `,0x,0x,0x1f` and the
compiler stops at "invalid suffix 'x' on integer constant".

That could have been a second `brew install` (`coreutils`, for its
`god`). It is not, because the normalization is three substitutions and
requiring a whole package for one command in one build step is the wrong
trade. `tools/build-toybox.sh` writes a shim and puts it first on PATH
for the duration of that build. The distinction worth keeping: `gsed` is
a *requirement of somebody else's build* and is listed above as one; the
`od` difference is a fact about the machine doing the building, and
belongs in the script that does the building.

## M24: the UEFI toolchain is a separate, parallel one

Everything above builds the OS itself (ELF64 kernel/user binaries, the
flat `mbr.bin`/`kernel.bin` blobs). `kernel/boot/uefi/boot.c` builds to a *PE32+* EFI
application instead — a hard requirement of the UEFI spec, not a choice —
so it needs its own compiler target (`clang -target x86_64-unknown-windows`)
and its own linker (`lld-link`, not `x86_64-elf-ld`). See that file's own
header comment for why hand-written headers instead of GNU-EFI/edk2, and
`tools/build-ovmf.sh`'s header comment for why OVMF (the firmware QEMU
needs to actually *test* that boot path) has to be built from source here
rather than `brew install`ed like everything else in this table.

## Why a real cross-compiler instead of Apple clang

Apple's `clang` can target `x86_64`, but it links with `ld64`, which
doesn't produce the flat/ELF binaries a bare-metal OS needs and doesn't
give us clean control over `-ffreestanding -nostdlib`. A real
`x86_64-elf` cross toolchain (GCC + binutils built for that target, no
host-OS assumptions baked in) is the standard OSDev approach and avoids
fighting the host toolchain's assumptions about linking against a libc or
dynamic loader.

## Why NASM over GAS

Binutils' `as` (GAS) uses AT&T syntax and comes "free" with the
cross-binutils install. We're installing NASM anyway because Intel syntax
reads closer to the reference manuals and to essentially every
OSDev boot-sector tutorial — worth the one extra `brew install` given how
much of `kernel/boot` is hand-placed instructions.

## Verifying the setup

```sh
nasm -v
x86_64-elf-gcc --version
x86_64-elf-ld --version
qemu-system-x86_64 --version
```
