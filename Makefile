# lean_os top-level build
#
# Toolchain is dev-time only (see docs/toolchain.md) — nothing here ships
# inside the OS image.

AS      := nasm
CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy
NM      := x86_64-elf-nm
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

# M39: a second host program, same arrangement - tools/gen-font.c holds the
# single glyph source both copies of the 8x16 font table are generated from
# (kernel/drivers/font8x16.{h,c} and user_space/lib/font8x16.{h,c}), plus the
# metric checks that keep them honest. M57 added the proportional UI font
# family to the same generator (user_space/lib/uifont.{h,c}) - three sizes on
# one shared baseline, checked the same way and by the same program, because
# "they read as one family" is exactly the property that quietly stops being
# true otherwise. The generated files stay checked in, so
# an ordinary build never needs to run this; what the stamp below guarantees is
# that they can't be edited by hand - or drift apart - without the build
# noticing. See tools/gen-font.c's header for why the duplication itself is
# unavoidable.
GEN_FONT     := $(BUILD)/gen-font
FONT_STAMP   := $(BUILD)/.font-check-stamp
FONT_FILES   := kernel/drivers/font8x16.h kernel/drivers/font8x16.c \
                user_space/lib/font8x16.h user_space/lib/font8x16.c \
                user_space/lib/uifont.h user_space/lib/uifont.c

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
# -MMD -MP: M48. Until this milestone nothing in this build depended on a
# header, so editing one recompiled nothing - and a struct that changed
# size (task_t has done it twice) left every translation unit that wasn't
# also touched linking against the old layout. That is a silent,
# arbitrarily-weird class of bug, and it cost real time here: raising
# MAX_TASKS in sched.h produced a kernel that still logged the old cap.
# -MMD writes a .d file of each object's real header dependencies beside
# it; -MP adds phony targets so deleting a header doesn't wedge the build.
CFLAGS := -std=c11 -O1 -ffreestanding -fno-stack-protector -fno-pic \
          -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror \
          -MMD -MP -Ikernel -Isystem_api/include -c

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
# M63: -mgeneral-regs-only is gone from *user* code and stays in CFLAGS
# above for the kernel, which is the whole reason FPU state is cheap here
# (kernel/arch/x86_64/fpu.h has the full argument): a kernel that never
# touches SSE never has to save it on an interrupt, so only a task switch
# does. Essentially no real C program compiles without `double`, so this
# one flag was the wall between this OS and running somebody else's
# program.
#
# -Iuser_space/libc/include puts <stdio.h>, <string.h>, <math.h> and the
# rest on the path for everything - our own programs included, which is
# deliberate: two string libraries in one tree is how they diverge. The
# cross-toolchain has no libc headers of its own, so nothing collides.
USER_CFLAGS := -std=c11 -O1 -ffreestanding -fno-stack-protector -fno-pic \
               -mcmodel=large -mno-red-zone -Wall -Wextra -Werror \
               -ffunction-sections -fdata-sections \
               -MMD -MP -Iuser_space/lib -Iuser_space/libc/include \
               -Isystem_api/include -c

MBR_BIN    := $(BUILD)/mbr.bin
KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
KERNEL_SECTORS_FILE := $(BUILD)/kernel.sectors
# M90: pages the loaded image occupies including .bss - see $(KERNEL_BIN).
KERNEL_PAGES_FILE   := $(BUILD)/kernel.pages
IMAGE      := $(BUILD)/os-image.bin
UEFI_BOOT_OBJ := $(BUILD)/uefi_boot.obj
UEFI_BOOT_EFI := $(BUILD)/BOOTX64.EFI

UOBJ      := $(BUILD)/user_obj
USER_LD   := user_space/lib/user.ld
USER_LIBOBJS := $(UOBJ)/crt0.o $(UOBJ)/syscall_wrappers.o $(UOBJ)/str.o $(UOBJ)/malloc.o \
                $(UOBJ)/gfx.o $(UOBJ)/font8x16.o $(UOBJ)/wmclient.o $(UOBJ)/wallpaper.o \
                $(UOBJ)/settings_file.o $(UOBJ)/children.o $(UOBJ)/icons.o \
                $(UOBJ)/uifont.o $(UOBJ)/recent.o $(UOBJ)/sntp.o $(UOBJ)/dns.o $(UOBJ)/http.o \
                $(UOBJ)/libc_string.o $(UOBJ)/libc_stdlib.o $(UOBJ)/libc_stdio.o \
                $(UOBJ)/libc_math.o $(UOBJ)/libc_time.o \
                $(UOBJ)/libc_env.o $(UOBJ)/libc_unistd.o \
                $(UOBJ)/libc_signal.o \
                $(UOBJ)/libc_dirent.o $(UOBJ)/libc_stat.o $(UOBJ)/libc_mman.o \
                $(UOBJ)/libc_pthread.o $(UOBJ)/libc_errno.o $(UOBJ)/libc_wchar.o $(UOBJ)/libc_locale.o \
                $(UOBJ)/libc_poll.o \
                $(UOBJ)/setjmp.o

# Every user program this project ships (M13): coreutils in bin/, plus
# init and shell in their own directories. Each becomes build/NAME.elf,
# linked against USER_LIBOBJS, and all of them get embedded into the
# kernel image together (kernel/proc/embed_programs.asm) - there's still
# no filesystem driver the *boot loader* can use to load from disk, only
# the kernel's own (M12), so this is still how anything gets onto the disk
# leanfs formats on first boot in the first place.
# M63: third-party programs are listed separately from this project's
# own, and built with their own flags (see THIRD_PARTY_CFLAGS) - the
# separation is the point. They are linked and embedded exactly like
# everything else, because "this desktop can run somebody else's program
# alongside its own" is only true if there is no special path for them.
THIRD_PARTY_PROGRAMS := whetstone

USER_PROGRAMS := hello echo cat cp ls audiograb libctest netconf nettime nettest tcptest racetest console nslookup fetch httpd caps captest init sh memtest fonttest compositor wm_demo gui_clock gui_paint desktop_shell desktop_icons gui_terminal text_editor file_manager settings task_manager wm_stubborn wm_zorder wm_faulter wm_crash badptr shutdown reboot env envtest sigtest treewalk mmaptest threadtest lazytest forktest exectest jobtest
USER_PROGRAMS += $(THIRD_PARTY_PROGRAMS)
USER_PROGRAM_ELFS := $(foreach p,$(USER_PROGRAMS),$(BUILD)/$(p).elf)

KERNEL_C_SRCS := $(shell find kernel -name '*.c' -not -path 'kernel/boot/*')
# ap_trampoline.asm is excluded here the same way kernel/boot/*.asm is: it's
# a standalone 16-bit flat binary (bits16, org 0x8000 - see its own header
# comment), not `-f elf64` kernel object code, so it gets its own build rule
# below instead of the normal pattern rule.
KERNEL_ASM_SRCS := $(shell find kernel -name '*.asm' -not -path 'kernel/boot/*' -not -name 'ap_trampoline.asm')

KERNEL_OBJS := $(patsubst kernel/%.asm,$(KOBJ)/%.o,$(KERNEL_ASM_SRCS)) \
               $(patsubst kernel/%.c,$(KOBJ)/%.o,$(KERNEL_C_SRCS))

.PHONY: all run leanfs-put preseed font font-check clean

all: $(IMAGE)

$(BUILD) $(KOBJ):
	mkdir -p $@

# M83: depends on this Makefile, not only on the source.
#
# The two numbers baked into this binary come from variables *here*, and
# without that dependency changing them rebuilds nothing. Moving the ESP
# in M83 produced exactly that: a correct image with a stale partition
# entry pointing at where the ESP used to be, and a firmware that reported
# "No bootable option or device was found" - a failure with no visible
# connection to the one-line edit that caused it.
$(MBR_BIN): $(BOOT)/mbr.asm Makefile | $(BUILD)
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

# M63: the libc subset. Prefixed object names rather than a second
# pattern rule on a bare basename, because `string.c` and `str.c` would
# otherwise be one namespace away from each other in $(UOBJ) - and the
# whole point of these files is that they are a *different* layer from
# user_space/lib, not a rename of it.
$(UOBJ)/libc_%.o: user_space/libc/src/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

# M63: third-party source, built with the warning flags relaxed and
# nothing else changed.
#
# -Werror is dropped and that is the honest thing to do rather than a
# shortcut: this code was written decades before these warnings existed
# and its author is not going to fix them, so treating them as errors
# would mean *editing somebody else's program* to make it build - which
# is precisely what docs/third-party-programs.md exists to keep to a
# minimum. -Wno-format-security is the one specific silence needed:
# whetstone.c passes a `const char *` as printf's whole format string,
# which is a warning about a pattern and not about a bug here.
THIRD_PARTY_CFLAGS := $(filter-out -Wall -Wextra -Werror,$(USER_CFLAGS)) -Wno-format-security

$(UOBJ)/whetstone.o: third_party/whetstone/whetstone.c | $(UOBJ)
	$(CC) $(THIRD_PARTY_CFLAGS) $< -o $@

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
	$(LD) --gc-sections -T $(USER_LD) -o $@ $(USER_LIBOBJS) $(UOBJ)/$*.o

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

# M41: every name in USER_PROGRAMS has to appear in embed_programs.asm too,
# or the kernel gets a dangling `extern NAME_elf_start` and the *link*
# fails - a real error, but one that reads as a wall of undefined-reference
# lines several build steps away from the one-word list that actually
# caused it. (Adding menu_bar without this check cost a genuinely
# confusing debugging detour: the stale image from the last good build
# stayed on disk and kept booting, so every test kept exercising the old
# kernel.) Failing here instead names the missing program and the file to
# add it to.
$(KOBJ)/proc/embed_programs.o: $(USER_PROGRAM_ELFS) | check-embedded-programs

.PHONY: check-embedded-programs
check-embedded-programs:
	@for p in $(USER_PROGRAMS); do \
	  grep -q "^$${p}_elf_start:" kernel/proc/embed_programs.asm || { \
	    echo "Makefile: '$$p' is in USER_PROGRAMS but has no incbin block in kernel/proc/embed_programs.asm." >&2; \
	    echo "          Add one (see that file's header comment on why it is written out longhand)." >&2; \
	    exit 1; }; \
	done

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
	           -DKERNEL_IMAGE_PAGES=$$(cat $(KERNEL_PAGES_FILE)) \
	           -Ikernel/boot/uefi -c kernel/boot/uefi/boot.c -o $@

$(UEFI_BOOT_EFI): $(UEFI_BOOT_OBJ)
	$(UEFI_LINK) /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:X64 \
	             /dll /dynamicbase:no /out:$@ $<

# Pads kernel.bin up to a whole number of 512-byte sectors (so the image
# layout is exact — no relying on how a short final sector reads off
# disk) and records that sector count for boot.c to read the kernel back
# with.
#
# M90: also records how many 4 KiB pages the *loaded* image occupies,
# which is not the same number. .bss is NOBITS, so it is absent from
# kernel.bin and from the sector count above - and the loader was
# reserving only the sectors it reads, leaving every byte of .bss outside
# any AllocatePages call. Firmware was free to put its own pool
# allocations there, and entry.asm zeroes the whole region before
# kernel_main runs: the e820 handoff buffer is an EfiLoaderData pool
# allocation, so the failure mode was the memory map being erased by the
# kernel that was about to read it. It never fired because .bss was small
# and the pool happened to land elsewhere. M90 makes .bss smaller rather
# than larger (the frame bitmap stops being a static array), which is not
# a fix - this is.
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -f%z $@); \
	sectors=$$(( (size + 511) / 512 )); \
	padded=$$(( sectors * 512 )); \
	truncate -s $$padded $@; \
	echo $$sectors > $(KERNEL_SECTORS_FILE); \
	end=$$($(NM) $(KERNEL_ELF) | awk '$$3 == "__kernel_end" { print $$1 }'); \
	[ -n "$$end" ] || { echo "kernel.bin: __kernel_end not found in $(KERNEL_ELF)" >&2; exit 1; }; \
	echo $$(( ( (0x$$end - 0x100000) + 4095 ) / 4096 )) > $(KERNEL_PAGES_FILE)

# leanfs (kernel/fs/leanfs.c) starts at sector 2048 (1 MiB) and needs
# 1 superblock + INODE_TABLE_SECTORS + 16 bitmap + 65536 data sectors,
# matching leanfs.c/leanfs.h's own layout constants (M15 grew this from
# the original 4105 to add indirect-block support and a much bigger data
# region; M74-M79's LEANFS_MAX_INODES 96 -> 192 took the inode table from
# 16 sectors to 32). The boot image (mbr.bin+kernel.bin) has to stay well
# clear of that, and the disk file itself has to actually be big enough
# to hold the whole filesystem region, or QEMU has nothing there for the
# ATA driver to read/write.
#
# Only FS_START_LBA is used by a recipe (the boot-image size guard
# below); the total is here as documentation of what has to fit between
# it and ESP_START_LBA, and 2048 + 65585 = 67633 leaves the ESP's 69632
# nearly two thousand sectors of room.
# M83: 2048 -> 8192, and FS_TOTAL_SECTORS corrected.
#
# Two things were wrong at once and only one of them announced itself.
#
# The one that did: the boot image (MBR + kernel) reached 2049 sectors and
# the guard below refused to build, which is exactly what the guard is for.
# 2048 sectors is 1 MiB and the kernel had been comfortably under it for
# eighty milestones; two more embedded programs and M81-M83's code took it
# over. 8192 gives 4 MiB, which is four times the current size rather than
# the 1.0004 times 2049 had left.
#
# The one that did not: FS_TOTAL_SECTORS said 65585, which was right when
# the inode table was 32 sectors. M81 made it 2048, so the real total is
# 1 + 2048 + 16 + 65536 = 67601 - and 2048 + 67601 = 69649, which is 17
# sectors PAST where the ESP started. The filesystem and the EFI System
# Partition had been overlapping since M81. Nothing failed, because the
# ESP is rewritten by every `make` and those 17 blocks are at the very end
# of a 32 MiB data region that nothing had filled - which is precisely the
# kind of bug that waits for the day somebody's disk is full. This number
# is a comment that has to be kept true by hand; it is now also the one
# that sizes ESP_START_LBA below, so a future drift moves the ESP with it
# instead of silently overlapping.
FS_START_LBA     := 8192
FS_TOTAL_SECTORS := 67601
IMAGE_SECTORS    := 77824

# The EFI System Partition the UEFI firmware boots from - kernel/boot/mbr.asm's
# partition entry hardcodes these same two numbers (passed in via `nasm -D`,
# see $(MBR_BIN)'s recipe above) and must move with them if they ever change
# here. Unlike leanfs, nothing here needs to persist rebuild to rebuild, so
# every `make` reformats it from scratch alongside BOOTX64.EFI.
#
# M45: moved from LBA 1024 to past the end of the filesystem. It used to sit
# in the 1024-sector gap between the boot blob and leanfs, and the kernel
# grew into it - the guard below caught that at exactly 1025 sectors, which
# is the guard doing its job but also a wall one milestone's worth of code
# away. There is nothing that wants the ESP *before* the filesystem, and
# moving it to the end turns that 1024-sector ceiling into a 2047-sector one
# (the kernel now only has to stay clear of FS_START_LBA), at the cost of
# 2048 more sectors of image file. IMAGE_SECTORS above moved with it.
# M83: derived from FS_START_LBA + FS_TOTAL_SECTORS rather than written
# out, so it cannot be left behind by a filesystem that grows - which is
# exactly what happened between M81 and here. Rounded up to a whole
# mebibyte for legibility in a hex dump.
ESP_START_LBA    := 76800
ESP_SECTOR_COUNT := 1024

$(IMAGE): $(MBR_BIN) $(KERNEL_BIN) $(UEFI_BOOT_EFI)
	@boot_sectors=$$(( ($$(stat -f%z $(MBR_BIN)) + $$(stat -f%z $(KERNEL_BIN))) / 512 )); \
	if [ $$boot_sectors -ge $(FS_START_LBA) ]; then \
		echo "error: boot image ($$boot_sectors sectors) has grown into the filesystem's start (LBA $(FS_START_LBA)) - move FS_START_LBA out further here and in kernel/fs/leanfs.c's LEANFS_START_LBA" >&2; \
		exit 1; \
	fi; \
	fs_end=$$(( $(FS_START_LBA) + $(FS_TOTAL_SECTORS) )); \
	if [ $$fs_end -gt $(ESP_START_LBA) ]; then \
		echo "error: the filesystem ends at LBA $$fs_end, past the ESP's start ($(ESP_START_LBA)) - they overlap. Move ESP_START_LBA out and IMAGE_SECTORS with it." >&2; \
		exit 1; \
	fi; \
	if [ $$(( $(ESP_START_LBA) + $(ESP_SECTOR_COUNT) )) -gt $(IMAGE_SECTORS) ]; then \
		echo "error: the ESP runs past the end of the image - grow IMAGE_SECTORS." >&2; \
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

$(GEN_FONT): tools/gen-font.c | $(BUILD)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -o $@ $<

# Regenerate the four font files from tools/gen-font.c's glyph source. The
# only supported way to change the font - editing a generated table by hand
# fails font-check below on the next build.
font: $(GEN_FONT)
	@$(GEN_FONT) --write

font-check: $(GEN_FONT)
	@$(GEN_FONT) --check

# The stamp is what actually wires the check into an ordinary `make`: both
# font8x16.o objects depend on it, so neither can be compiled while a
# checked-in table disagrees with the generator (or with the other copy -
# they're emitted from the same array, so matching the generator *is* the
# byte-identity guarantee). A stamp file rather than a .PHONY prerequisite
# so this stays incremental and doesn't relink the kernel on every build.
$(FONT_STAMP): tools/gen-font.c $(FONT_FILES) $(GEN_FONT) | $(BUILD)
	@$(GEN_FONT) --check
	@touch $@

$(KOBJ)/drivers/font8x16.o: $(FONT_STAMP)
$(UOBJ)/font8x16.o: $(FONT_STAMP)
$(UOBJ)/uifont.o: $(FONT_STAMP)

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
		$(LEANFS_PUT) $(IMAGE) $(BUILD)/$$p.elf /bin/$$p; \
	done

# Lets tools/build-user-program.sh (and anyone else) read this Makefile's
# own variables - e.g. `make print-USER_CFLAGS` - instead of hardcoding a
# second copy of flags that would silently drift out of sync with the
# ones actually used to build this project's own user-space programs.
print-%:
	@echo $($*)

# The .d files -MMD leaves beside every object (see CFLAGS above). Included
# with a leading `-` so a from-scratch build, where none of them exist yet,
# doesn't warn about every one.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(BUILD)
