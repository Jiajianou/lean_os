# lean_os top-level build
#
# Toolchain is dev-time only (see docs/toolchain.md) — nothing here ships
# inside the OS image.

AS      := nasm
CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy
QEMU    := qemu-system-x86_64

# M24 (UEFI boot path): a completely separate toolchain from the rest of
# this project (see kernel/boot/uefi/boot.c's header comment) - clang
# targeting a PE/COFF triple plus lld's PE-mode linker, because a UEFI
# application has to *be* a PE32+ binary, not an ELF one. Still dev-time
# only, still nothing shipped in the OS image (this compiles to
# BOOTX64.EFI, firmware-loaded scaffolding that hands off to the exact
# same kernel.bin the BIOS path builds - it never becomes part of the OS
# itself). `brew install lld` if lld-link isn't already on PATH.
UEFI_CC      := clang
UEFI_CC_TARGET := x86_64-unknown-windows
UEFI_LINK    := lld-link
MFORMAT      := mformat
MMD          := mmd
MCOPY        := mcopy

BUILD := build
BOOT  := kernel/boot
KOBJ  := $(BUILD)/kernel_obj

# Package/build tooling stretch goal: a *host* program (ordinary libc,
# built with the host's own cc, not the x86_64-elf cross-compiler - it
# never runs as part of the OS) that writes a file straight into an
# already-built disk image's leanfs filesystem. See its own header
# comment and tools/build-user-program.sh for the full third-party
# program workflow this exists for.
HOSTCC       := cc
LEANFS_PUT   := $(BUILD)/leanfs-put

# -O1: added at M20 - a from-scratch software compositor doing
# per-pixel fill_rect/blit calls at -O0 turned out genuinely too slow to
# be usable (verified directly during M20 bring-up: a single full-screen
# redraw took close to a second, unoptimized function-call overhead per
# pixel dominating). -O1 is enough to get GCC inlining these small static
# helpers without pulling in anything that would fight -ffreestanding.
# -mgeneral-regs-only: required alongside it, not optional polish - at
# -O1 GCC started auto-vectorizing/optimizing some code into SSE
# instructions (struct copies etc.), and this kernel never sets up FPU/
# SSE state (CR0/CR4 OSFXSR and friends), so the first one executed
# faulted as an invalid opcode. Caught by an actual boot panic during
# M20 verification, not anticipated in advance - this flag forces
# scalar/GPR-only codegen, the standard freestanding-kernel fix for
# exactly this.
CFLAGS := -std=c11 -O1 -ffreestanding -fno-stack-protector -fno-pic \
          -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror \
          -Ikernel -Isystem_api/include -c

# user_space code is freestanding for the same reasons kernel code is (see
# milestones.md's ground rules) but has its own include root (its own
# lib/) instead of kernel/. -mcmodel=large: user.ld loads programs at
# 512 GiB (PML4[1], well clear of the kernel's low-1-GiB identity map -
# see user.ld's header comment) - the default "small" code model can't
# generate relocations for absolute addresses that far out (a plain
# -fno-pic build fails with "relocation truncated to fit" the moment code
# takes the address of anything in .rodata/.data), and PIC/PLT machinery
# would be pure overhead for statically-linked, position-*dependent*
# binaries like these.
USER_CFLAGS := -std=c11 -O1 -ffreestanding -fno-stack-protector -fno-pic \
               -mcmodel=large -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror \
               -Iuser_space/lib -Isystem_api/include -c

MBR_BIN    := $(BUILD)/mbr.bin
KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
KERNEL_SECTORS_FILE := $(BUILD)/kernel.sectors
IMAGE      := $(BUILD)/os-image.bin
UEFI_BOOT_OBJ := $(BUILD)/uefi_boot.obj
UEFI_BOOT_EFI := $(BUILD)/BOOTX64.EFI

UOBJ      := $(BUILD)/user_obj
USER_LD   := user_space/lib/user.ld
USER_LIBOBJS := $(UOBJ)/crt0.o $(UOBJ)/syscall_wrappers.o $(UOBJ)/str.o $(UOBJ)/malloc.o \
                $(UOBJ)/gfx.o $(UOBJ)/font8x16.o $(UOBJ)/wmclient.o

# Every user program this project ships (M13): coreutils in bin/, plus
# init and shell in their own directories. Each becomes build/NAME.elf,
# linked against USER_LIBOBJS, and all of them get embedded into the
# kernel image together (kernel/proc/embed_programs.asm) - there's still
# no filesystem driver the *boot loader* can use to load from disk, only
# the kernel's own (M12), so this is still how anything gets onto the disk
# leanfs formats on first boot in the first place.
USER_PROGRAMS := hello echo cat ls init shell memtest compositor wm_demo gui_clock gui_paint desktop_shell desktop_icons gui_terminal text_editor file_manager settings
USER_PROGRAM_ELFS := $(foreach p,$(USER_PROGRAMS),$(BUILD)/$(p).elf)

KERNEL_C_SRCS := $(shell find kernel -name '*.c' -not -path 'kernel/boot/*')
# ap_trampoline.asm is excluded here the same way kernel/boot/*.asm is: it's
# a standalone 16-bit flat binary (bits16, org 0x8000 - see its own header
# comment), not `-f elf64` kernel object code, so it gets its own build rule
# below instead of the normal pattern rule.
KERNEL_ASM_SRCS := $(shell find kernel -name '*.asm' -not -path 'kernel/boot/*' -not -name 'ap_trampoline.asm')

KERNEL_OBJS := $(patsubst kernel/%.asm,$(KOBJ)/%.o,$(KERNEL_ASM_SRCS)) \
               $(patsubst kernel/%.c,$(KOBJ)/%.o,$(KERNEL_C_SRCS))

.PHONY: all run leanfs-put preseed clean

all: $(IMAGE)

$(BUILD) $(KOBJ):
	mkdir -p $@

$(MBR_BIN): $(BOOT)/mbr.asm | $(BUILD)
	$(AS) -f bin -D ESP_START_LBA=$(ESP_START_LBA) -D ESP_SECTOR_COUNT=$(ESP_SECTOR_COUNT) $< -o $@

$(KOBJ)/%.o: kernel/%.asm | $(KOBJ)
	@mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

$(KOBJ)/%.o: kernel/%.c | $(KOBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(UOBJ):
	mkdir -p $@

$(UOBJ)/%.o: user_space/lib/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

$(UOBJ)/%.o: user_space/lib/%.asm | $(UOBJ)
	$(AS) -f elf64 $< -o $@

$(UOBJ)/%.o: user_space/bin/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

$(UOBJ)/%.o: user_space/init/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

$(UOBJ)/%.o: user_space/shell/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

# Each user program: built and linked entirely on its own (real ET_EXEC
# ELF64 output, not objcopy'd flat - kernel/proc/elf.c parses these
# program headers directly) against user_space/lib. embed_programs.o
# needs every one of these .elf files to exist before nasm can assemble
# it - an explicit extra prerequisite on top of the normal *.asm pattern
# rule below, the same "side effect a generic rule wouldn't know to
# guarantee" situation $(UEFI_BOOT_OBJ)'s kernel.sectors dependency below is.
$(BUILD)/%.elf: $(UOBJ)/%.o $(USER_LIBOBJS) $(USER_LD)
	$(LD) -T $(USER_LD) -o $@ $(USER_LIBOBJS) $(UOBJ)/$*.o

# AP_TRAMPOLINE_BIN: the standalone 16-bit SMP AP bring-up blob (see
# kernel/arch/x86_64/ap_trampoline.asm's header comment) - built like
# mbr.bin (flat `-f bin`, no ELF, no linking), then incbin'd into the
# kernel image by embed_ap_trampoline.o, the same "explicit extra
# prerequisite the generic *.asm pattern rule wouldn't know to guarantee"
# situation embed_programs.o's USER_PROGRAM_ELFS dependency is already in.
AP_TRAMPOLINE_BIN := $(BUILD)/ap_trampoline.bin

$(AP_TRAMPOLINE_BIN): kernel/arch/x86_64/ap_trampoline.asm | $(BUILD)
	$(AS) -f bin $< -o $@

$(KOBJ)/proc/embed_ap_trampoline.o: $(AP_TRAMPOLINE_BIN)

$(KOBJ)/proc/embed_programs.o: $(USER_PROGRAM_ELFS)

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	$(LD) -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

# BOOTX64.EFI needs KERNEL_SECTOR_COUNT to know how many sectors to read
# the kernel blob back from disk - only known once the kernel is actually
# built (see $(KERNEL_BIN)'s recipe below, which generates
# $(KERNEL_SECTORS_FILE) as a side effect of computing it).
$(UEFI_BOOT_OBJ): kernel/boot/uefi/boot.c kernel/boot/uefi/efi.h kernel/boot/uefi/efi_proto.h $(KERNEL_BIN) | $(BUILD)
	$(UEFI_CC) -target $(UEFI_CC_TARGET) -ffreestanding -fshort-wchar -mno-red-zone \
	           -fno-stack-protector -std=c11 -Wall -Wextra -Werror \
	           -DKERNEL_SECTOR_COUNT=$$(cat $(KERNEL_SECTORS_FILE)) \
	           -Ikernel/boot/uefi -c kernel/boot/uefi/boot.c -o $@

$(UEFI_BOOT_EFI): $(UEFI_BOOT_OBJ)
	$(UEFI_LINK) /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:X64 \
	             /dll /dynamicbase:no /out:$@ $<

# Pads kernel.bin up to a whole number of 512-byte sectors (so the image
# layout is exact — no relying on how a short final sector reads off
# disk) and records that sector count for boot.c to read the kernel back
# with.
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -f%z $@); \
	sectors=$$(( (size + 511) / 512 )); \
	padded=$$(( sectors * 512 )); \
	truncate -s $$padded $@; \
	echo $$sectors > $(KERNEL_SECTORS_FILE)

# leanfs (kernel/fs/leanfs.c) starts at sector 2048 (1 MiB) and needs
# 65560 sectors (1 superblock + 7 inode-table + 16 bitmap + 65536 data,
# matching leanfs.c/leanfs.h's own layout constants exactly - if those
# ever change, this has to move with them; M15 grew this from the
# original 4105 to add indirect-block support and a much bigger data
# region). The boot image (mbr.bin+kernel.bin) has to stay well clear of
# that, and the disk file itself has to actually be big enough to hold the
# whole filesystem region, or QEMU has nothing there for the ATA driver to
# read/write.
FS_START_LBA     := 2048
FS_TOTAL_SECTORS := 65560
IMAGE_SECTORS    := 69632

# The EFI System Partition the UEFI firmware boots from - kernel/boot/mbr.asm's
# partition entry hardcodes these same two numbers (passed in via `nasm -D`,
# see $(MBR_BIN)'s recipe above) and must move with them if they ever change
# here. Sits in the same "boot blob has to stay clear of this" gap
# FS_START_LBA already carves out below; unlike leanfs, nothing here needs
# to persist rebuild to rebuild, so every `make` reformats it from scratch
# alongside BOOTX64.EFI.
ESP_START_LBA    := 1024
ESP_SECTOR_COUNT := 1024

$(IMAGE): $(MBR_BIN) $(KERNEL_BIN) $(UEFI_BOOT_EFI)
	@boot_sectors=$$(( ($$(stat -f%z $(MBR_BIN)) + $$(stat -f%z $(KERNEL_BIN))) / 512 )); \
	if [ $$boot_sectors -ge $(ESP_START_LBA) ]; then \
		echo "error: boot image ($$boot_sectors sectors) has grown into the ESP's start (LBA $(ESP_START_LBA)) - move ESP_START_LBA out further here and in mbr.asm" >&2; \
		exit 1; \
	fi
	cat $(MBR_BIN) $(KERNEL_BIN) > $(IMAGE)
	truncate -s $$(( $(IMAGE_SECTORS) * 512 )) $(IMAGE)
	$(MFORMAT) -i $(IMAGE)@@$$(( $(ESP_START_LBA) * 512 )) -T $(ESP_SECTOR_COUNT)
	$(MMD) -i $(IMAGE)@@$$(( $(ESP_START_LBA) * 512 )) ::EFI
	$(MMD) -i $(IMAGE)@@$$(( $(ESP_START_LBA) * 512 )) ::EFI/BOOT
	$(MCOPY) -i $(IMAGE)@@$$(( $(ESP_START_LBA) * 512 )) -o $(UEFI_BOOT_EFI) ::EFI/BOOT/BOOTX64.EFI

# tools/run-qemu.sh does its own `make all` (and builds OVMF firmware via
# tools/build-ovmf.sh first if it isn't present yet), so `run` doesn't need
# `all` as a prerequisite here - the script is the single source of truth
# for "build then boot."
run:
	@./tools/run-qemu.sh

$(LEANFS_PUT): tools/leanfs-put.c | $(BUILD)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -o $@ $<

leanfs-put: $(LEANFS_PUT)

# Optional, one-time-per-image step for the third-party program workflow
# (tools/build-user-program.sh + tools/leanfs-put.c): writes every
# built-in program straight into the disk image's leanfs, host-side, in
# the same order kernel.c's own FOR_EACH_EMBEDDED_PROGRAM seeds them in.
#
# Not part of `all` and never runs automatically - default boot behavior
# (the kernel seeding these itself on first boot, M12) is untouched
# either way, since it already skips any program that's already present.
# The reason this exists at all: kernel.c's M22 self-test hardcodes
# "launcher slot 0 is hello, the first file ever seeded" and checks
# actual rendered pixels for it: leanfs-put'ing a third-party program
# onto a disk image that has *never* booted claims the first free inode
# for itself, which - if that inode happens to be slot 0 - shifts hello
# out of it and fails that self-test. Running this once first, before
# adding any third-party program to a fresh image, avoids the question
# entirely by making sure every built-in program's slot is already
# spoken for.
preseed: $(IMAGE) $(LEANFS_PUT)
	@for p in $(USER_PROGRAMS); do \
		$(LEANFS_PUT) $(IMAGE) $(BUILD)/$$p.elf $$p; \
	done

# Lets tools/build-user-program.sh (and anyone else) read this Makefile's
# own variables - e.g. `make print-USER_CFLAGS` - instead of hardcoding a
# second copy of flags that would silently drift out of sync with the
# ones actually used to build this project's own user-space programs.
print-%:
	@echo $($*)

clean:
	rm -rf $(BUILD)
