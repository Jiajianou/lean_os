# Toolchain

Dev-time tools only — nothing here ships inside the OS image (see the
ground rules in [milestones.md](../milestones.md)).

| Tool | Version | Purpose | Installed via |
|---|---|---|---|
| `x86_64-elf-gcc` | 16.2.0 | freestanding C compiler | `brew install x86_64-elf-gcc` |
| `x86_64-elf-ld` | binutils 2.47 | linker, uses our own linker scripts | `brew install x86_64-elf-binutils` |
| `nasm` | 3.02 | assembler for boot code and other `.asm` | `brew install nasm` |
| `qemu-system-x86_64` | 10.1.3 | emulator for fast boot testing | already present |

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
