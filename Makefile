# lean_os top-level build
#
# Toolchain is dev-time only (see docs/toolchain.md) — nothing here ships
# inside the OS image.

# Q10: overridable from the environment, but only from the environment.
#
# These were `:=`, which beats the environment in GNU make and made the
# toolchain unchangeable without editing this file. That is fine on a
# developer's machine and exactly wrong on a machine where x86_64-elf-gcc
# is not installed and a distribution's x86_64-linux-gnu cross-compiler,
# which produces the same freestanding objects for these flags, is.
#
# `?=` is NOT the fix and was tried first: AS, CC and LD are built-in make
# variables with default values, so `?=` sees them as already defined and
# does nothing - which silently left AS as `as` and handed nasm's
# arguments to clang. Testing the *origin* is the version that works:
# `default` means make invented it and we should override; `environment`
# or `command line` means somebody chose it and we should not.
ifeq ($(origin AS),default)
AS      := nasm
endif
ifeq ($(origin CC),default)
CC      := x86_64-elf-gcc
endif
ifeq ($(origin LD),default)
LD      := x86_64-elf-ld
# M94: the archiver, for the sysroot's libc.a. The cross one, not the
# host's: an archive of ELF64 objects indexed by a Mach-O ar is an
# archive x86_64-elf-ld will not read.
AR      := x86_64-elf-ar
endif
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
# M94: crti.o first and crtn.o last, with everything else between.
# _init and _fini are built out of fragments and are a function only
# because of link order - see user_space/lib/crti.asm.
USER_LIBOBJS := $(UOBJ)/crt0.o $(UOBJ)/crti.o $(UOBJ)/syscall_wrappers.o $(UOBJ)/str.o $(UOBJ)/malloc.o \
                $(UOBJ)/gfx.o $(UOBJ)/font8x16.o $(UOBJ)/wmclient.o $(UOBJ)/wallpaper.o \
                $(UOBJ)/settings_file.o $(UOBJ)/children.o $(UOBJ)/icons.o \
                $(UOBJ)/uifont.o $(UOBJ)/recent.o $(UOBJ)/sntp.o $(UOBJ)/dns.o $(UOBJ)/http.o \
                $(UOBJ)/libc_string.o $(UOBJ)/libc_stdlib.o $(UOBJ)/libc_stdio.o \
                $(UOBJ)/libc_math.o $(UOBJ)/libc_time.o \
                $(UOBJ)/libc_env.o $(UOBJ)/libc_unistd.o \
                $(UOBJ)/libc_signal.o \
                $(UOBJ)/libc_dirent.o $(UOBJ)/libc_stat.o $(UOBJ)/libc_mman.o \
                $(UOBJ)/libc_pthread.o $(UOBJ)/libc_errno.o $(UOBJ)/libc_wchar.o $(UOBJ)/libc_locale.o \
                $(UOBJ)/libc_poll.o $(UOBJ)/libc_resource.o \
                $(UOBJ)/libc_statvfs.o $(UOBJ)/libc_utime.o $(UOBJ)/libc_pwd.o \
                $(UOBJ)/libc_termios.o $(UOBJ)/libc_grp.o $(UOBJ)/libc_libgen.o \
                $(UOBJ)/libc_fnmatch.o $(UOBJ)/libc_strings.o $(UOBJ)/libc_sysinfo.o \
                $(UOBJ)/libc_regex.o $(UOBJ)/libc_syslog.o \
                $(UOBJ)/libc_socket.o $(UOBJ)/libc_netdb.o \
                $(UOBJ)/libc_wctype.o $(UOBJ)/libc_ctype.o \
                $(UOBJ)/libc_fcntl.o $(UOBJ)/libc_scanf.o $(UOBJ)/libc_mntent.o \
                $(UOBJ)/libc_xattr.o $(UOBJ)/libc_klog.o $(UOBJ)/libc_getopt.o $(UOBJ)/libc_reboot.o $(UOBJ)/libc_tls.o \
                $(UOBJ)/libc_pty.o $(UOBJ)/libc_select.o $(UOBJ)/libc_realpath.o $(UOBJ)/libc_popen.o \
                $(UOBJ)/libc_iconv.o $(UOBJ)/libc_iconv_tables.o \
                $(UOBJ)/sha256.o $(UOBJ)/ospkg.o $(UOBJ)/fsutil.o \
                $(UOBJ)/setjmp.o $(UOBJ)/symtab.o $(UOBJ)/crtn.o

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

USER_PROGRAMS := hello echo cat cp ls audiograb libctest netconf nettime nettest tcptest racetest console nslookup fetch httpd caps captest init sh memtest fonttest compositor wm_demo gui_clock gui_paint desktop_shell desktop_icons gui_terminal text_editor file_manager settings task_manager wm_stubborn wm_zorder wm_faulter wm_crash badptr shutdown reboot env envtest sigtest treewalk mmaptest threadtest lazytest vmtest forktest exectest jobtest syscalltest profile proftest oomtest futextest fswriter ptytest exhausttest measure faulttest os pkgtest dirtest browsertest
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

.PHONY: all run leanfs-put preseed toybox packages browser browser-if-built os-pkg sysroot print-user-programs syms font font-check clean distclean

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
# M101: crt0.o and setjmp.o are built by pattern rules and named only as
# prerequisites, which makes them *intermediate* files - so make deletes
# them after the first program links, and with -j8 the next link races
# the deletion. The failure is intermittent, and its shape is worse than
# its frequency: `make -j8 | grep error` reports nothing useful, the
# .elf silently keeps its previous contents, and the image boots the
# program you edited five minutes ago. That cost a three-minute QEMU run
# and a wrong conclusion about a test failure during this milestone.
#
# .SECONDARY with no prerequisites marks every target in this Makefile as
# secondary, which is the documented way to say "never auto-delete an
# intermediate". Naming just these two would work today and would go
# stale the next time something is added to USER_LIBOBJS by a pattern
# rule.
.SECONDARY:

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
#
# M93: leanfs blocks became 4096 bytes and the region grew from 32 MiB to
# 2 GiB, so this number is now computed rather than counted by hand -
# which is what the paragraph above spent forty lines wishing for. In
# leanfs blocks: 1 superblock + 4096 inode table (131072 inodes at 128
# bytes) + 16 bitmap + 524288 data = 528401, and a block is 8 sectors.
#
#     528401 * 8 = 4227208 sectors = 2.016 GiB
#
# The image file that holds it is sparse - `truncate` creates it, nothing
# writes the data region until something stores a file there, and a first
# boot's format touches only the 16 MiB inode table and the 64 KiB
# bitmap. So the cost of this on disk and in every harness's overlay is
# what is actually used, not two gigabytes.
FS_START_LBA     := 8192
FS_INODE_BLOCKS  := 4096
FS_BITMAP_BLOCKS := 16
FS_DATA_BLOCKS   := 524288
FS_TOTAL_SECTORS := $(shell echo $$(( (1 + $(FS_INODE_BLOCKS) + $(FS_BITMAP_BLOCKS) + $(FS_DATA_BLOCKS)) * 8 )))
IMAGE_SECTORS    := $(shell echo $$(( 8192 + (1 + 4096 + 16 + 524288) * 8 + 2048 )))

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
#
# M93: computed from FS_START_LBA + FS_TOTAL_SECTORS rather than written
# out, which M83's note above already said it should be ("so it cannot be
# left behind by a filesystem that grows - which is exactly what happened
# between M81 and here"). A filesystem that grew by sixty times is what
# finally made writing it out untenable.
ESP_START_LBA    := $(shell echo $$(( 8192 + (1 + 4096 + 16 + 524288) * 8 )))
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

# ---- M111: the package builder ----------------------------------------
#
# The host half of `os`. It shares user_space/lib/ospkg.c and sha256.c
# with the machine's own /bin/os, compiled here for this desk - so a
# package built by this tool and one read by that program are the same
# reader, which is the property that makes the format one format.
#
# -Iuser_space/lib and -Isystem_api/include because ospkg.c includes
# caps.h for the capability names: a package manifest says "network" and
# the kernel wants a bit, and there is one table that maps between them.
OS_PKG      := $(BUILD)/os-pkg
OS_PKG_SRCS := tools/os-pkg.c user_space/lib/ospkg.c user_space/lib/sha256.c

$(OS_PKG): $(OS_PKG_SRCS) user_space/lib/ospkg.h user_space/lib/sha256.h \
           system_api/include/caps.h | $(BUILD)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Iuser_space/lib \
	          -Isystem_api/include -o $@ $(OS_PKG_SRCS)

os-pkg: $(OS_PKG)

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

# ---- M89: toybox on the disk ------------------------------------------
#
# Builds the port (tools/build-toybox.sh, which is where every decision
# about it is written down) and installs it into the image: the binary at
# /bin/toybox, and one symbolic link per command name pointing at it.
#
# **The links are the install, and they are why leanfs-put grew a -s.**
# Toybox is a multi-call binary: it decides which command to be from
# argv[0], so `sort` has to be a name in /bin that resolves to this
# binary. A hundred and fifty copies would be sixty megabytes; a hundred
# and fifty links are a hundred and fifty inodes, which is what M87's
# symbolic links and M81's filesystem were for.
#
# ---- the collisions, and a correction to M89's third bullet ----------
#
# That bullet says "where toybox and this tree both provide a name, the
# one in /bin is the ported one and the lean_os one keeps its own name".
# Installed that way, the graded boot fails - and the failures are the
# argument for doing it the other way round:
#
#   /bin/httpd  - toybox's is a different program with different
#                 semantics, and M73's self-test fetches from lean_os's
#                 over loopback. The link replaced the server the test
#                 talks to.
#   /bin/sh     - the shell every script on this machine runs in,
#                 including M72's and M86's fixtures.
#   /bin/cp     - M60's self-test spawns it with two arguments and
#                 checks the copy it made.
#
# So the rule here is the reverse: **a name this project already ships
# keeps its program, and toybox's version of that name is reached as
# `toybox <name>`** - which is what a multi-call binary is for and costs
# nothing. Every name lean_os does NOT ship becomes a link, which is over
# a hundred of them and is the whole point of the milestone.
#
# The bullet's own reasoning survives the reversal: it said the overlap
# should stay "because `ls` here knows about leanfs's own shape and
# `caps` has no toybox equivalent". That is an argument for keeping the
# lean_os program; the sentence after it was a guess about which name
# should point where, made before the collision set was known. See
# milestones.md M89 for the corrected entry.
#
# Not part of `all`, for the same reason `preseed` is not: writing into a
# fresh image claims inodes that kernel.c's M22 self-test has opinions
# about. Run `make preseed` first on a new image, which this depends on
# so that the ordering is the build's problem and not a person's.
TOYBOX_BIN := $(BUILD)/toybox/toybox

$(TOYBOX_BIN): $(USER_PROGRAM_ELFS) tools/build-toybox.sh $(wildcard tools/toybox-port/*.patch)
	@./tools/build-toybox.sh

toybox: $(TOYBOX_BIN) $(IMAGE) $(LEANFS_PUT) preseed
	@$(LEANFS_PUT) $(IMAGE) $(TOYBOX_BIN) /bin/toybox
	@n=0; k=0; \
	for c in $$($(BUILD)/toybox/generated/unstripped/instlist); do \
		skip=0; \
		for p in $(USER_PROGRAMS); do \
			if [ "$$p" = "$$c" ]; then skip=1; break; fi; \
		done; \
		if [ $$skip -eq 1 ]; then k=$$((k+1)); continue; fi; \
		$(LEANFS_PUT) -s $(IMAGE) /bin/toybox /bin/$$c >/dev/null || exit 1; \
		n=$$((n+1)); \
	done; \
	echo "toybox: /bin/toybox plus $$n command names ($$k kept lean_os's own)"

# ---- M113: the browser, on the disk ------------------------------------
#
# M100 built NetSurf for this machine and left it in build/netsurf. What
# it did not do is put it on any image but the one that happened to be
# sitting there at the time - and `$(IMAGE)`'s recipe recreates the disk
# from scratch every time the kernel changes, so the browser was gone
# again after the next edit. That is the whole of M113: a browser you
# have to reinstall by hand after every kernel build is not installed.
#
# One command, because two script names nobody remembers is the same
# thing as no browser. This builds the port if it has never been built
# (twenty minutes, and it needs bison 3.x and the host's libpng - see
# tools/build-netsurf.sh) and installs it either way.
#
# Not part of `all`, for the same reason `preseed`, `toybox` and
# `packages` are not: writing into a fresh image claims inodes that
# kernel.c's M22 self-test has opinions about. Depends on preseed so the
# ordering is the build's problem rather than a person's.
#
# /bin/netsurf and not /pkg: this is a program the OS ships, so it is in
# the shipped capability table (system_api/include/caps.h) holding
# CAP_FS_WRITE | CAP_NETWORK. M111 made every binary under /pkg get zero
# capabilities whatever it is called, so a browser installed there could
# not open a socket. See docs/browser.md.
NETSURF_BIN := $(BUILD)/netsurf/netsurf

$(NETSURF_BIN): tools/build-netsurf.sh user_space/bin/nsfb_leanos.c
	@./tools/build-netsurf.sh

browser: $(NETSURF_BIN) $(IMAGE) $(LEANFS_PUT) preseed
	@./tools/install-netsurf.sh

# The same install without the build, for the callers that must not
# trigger a twenty-minute cross-compile: it puts the browser back on an
# image that has just been recreated, and says so and succeeds when the
# port has never been built. tools/run-qemu.sh and tools/run-tests.sh
# both go through this, which is what makes the browser survive a `make`.
browser-if-built: $(IMAGE) $(LEANFS_PUT)
	@if [ ! -f $(NETSURF_BIN) ]; then \
		echo "browser: not built - \`make browser\` builds and installs it (an image without a browser is a valid image)."; \
	else \
		$(MAKE) --no-print-directory preseed >/dev/null || exit 1; \
		./tools/install-netsurf.sh || exit 1; \
	fi

# ---- M111: the package repository, on the disk ------------------------
#
# `os install grep` reads /pkg/repo. This is what puts it there: the
# archives tools/build-packages.sh produced, plus the index, written into
# the image's leanfs host-side the same way `toybox` is.
#
# Not part of `all`, and for the same reason `preseed` and `toybox` are
# not: writing into a fresh image claims inodes that kernel.c's M22
# self-test has opinions about. Depends on preseed so the ordering is the
# build's problem rather than a person's.
#
# The repository directory is created here rather than by `os`, because
# an image with /pkg/repo and no packages in it and an image with no
# repository at all are different machines, and the first one is the one
# a person can put a package into.
PKG_REPO_DIR := $(BUILD)/repo

packages: $(IMAGE) $(LEANFS_PUT) preseed
	@if [ ! -f $(PKG_REPO_DIR)/index ]; then \
		echo "packages: no repository - run tools/build-packages.sh first" >&2; \
		exit 1; \
	fi
	@n=0; \
	for f in $(PKG_REPO_DIR)/*.osp $(PKG_REPO_DIR)/index; do \
		$(LEANFS_PUT) $(IMAGE) $$f /pkg/repo/$$(basename $$f) >/dev/null || exit 1; \
		n=$$((n+1)); \
	done; \
	echo "packages: $$n files into /pkg/repo in $(IMAGE)"

# ---- M94: the sysroot ---------------------------------------------------
#
# What a compiler needs in order to be told about this OS once, by name,
# instead of by a link line somebody types. `x86_64-lean_os-gcc hello.c
# -o hello` finds all of it through --sysroot, which is the entire point:
# **every flag invented by hand is a flag someone else's build system
# will not pass.**
#
#   usr/local/include   user_space/libc/include
#   usr/include         system_api/include
#   usr/lib             libc.a, crt1.o, crti.o, crtn.o, the linker script
#
# **Two directories and not one, and that is load-bearing.** `signal.h`
# and `termios.h` exist in BOTH header trees, and the libc one reaches
# the system_api one with `#include_next` - which continues the search
# from after wherever the first was found. Copied into one directory,
# the second copy simply overwrites the first and `#include_next` finds
# nothing: `<signal.h>` then declares `sighandler_t` and no `signal()`,
# and a program that includes it compiles until the line that calls one.
# That is what happened to bzip2, at bzip2.c:1808, and it is why the
# split is here rather than a tidier single directory.
#
# usr/local/include is searched BEFORE usr/include by this compiler (see
# `x86_64-lean_os-gcc -E -Wp,-v -`), which is the order those two
# headers need. Copied rather than symlinked so that a sysroot handed to
# somebody is a sysroot.
#
# **crt1.o and not crt0.o.** The file is called crt0.asm here and the ELF
# convention is crt1.o; the driver looks for the latter by name, so the
# sysroot uses the name the driver knows and this comment is where the
# two are reconciled.
#
# Generated by this target and never by hand, which is M94's fourth
# bullet and M25's lesson: a thing maintained by hand is a thing that is
# wrong by the time anybody looks.
SYSROOT := $(BUILD)/sysroot
LIBC_A  := $(BUILD)/libc.a

# Every object a program needs that is not its own and not a startup
# file. crt0/crti/crtn come out because the driver passes those
# separately and an archive member cannot be forced to a link position.
LIBC_A_OBJS := $(filter-out $(UOBJ)/crt0.o $(UOBJ)/crti.o $(UOBJ)/crtn.o,$(USER_LIBOBJS))

$(LIBC_A): $(LIBC_A_OBJS)
	@rm -f $@
	$(AR) rcs $@ $(LIBC_A_OBJS)

# ---- M97: the same library, as a shared object -----------------------
#
# M95 built one of these, inside tools/build-dynamic.sh, with the
# x86_64-lean_os compiler. This one is built with the same
# `x86_64-elf-gcc` that builds libc.a, and lives in the sysroot - and the
# difference is an ordering, not a preference.
#
# libstdc++.so has to link against a shared libc. If it does not find
# one it links the STATIC libc.a instead, and the first non-PIC object
# in it stops the link with "relocation R_X86_64_TPOFF32 against
# `lean_errno' can not be used when making a shared object". So the
# sysroot needs libc.so BEFORE the toolchain is built - and
# build-dynamic.sh cannot supply it, because it needs the toolchain that
# needs libc.so. One pass instead of two, by using the compiler that is
# already here.
#
# -fPIC and -mcmodel=small rather than the tree's usual -fno-pic and
# -mcmodel=large: a shared object is mapped wherever there is room and
# reaches itself RIP-relative. -ftls-model=initial-exec because a
# `__thread` variable in a library loaded at startup can use the fixed
# offset, and the general-dynamic model would need __tls_get_addr, which
# this system does not have.
LIBC_SO := $(BUILD)/libc.so
LIBC_SO_SRCS := $(wildcard user_space/libc/src/*.c) \
                user_space/lib/syscall_wrappers.c user_space/lib/str.c \
                user_space/lib/malloc.c user_space/lib/dns.c

# Linked with `x86_64-elf-ld` directly rather than through the driver,
# and for the reason CLAUDE.md already gives for the kernel: this is a
# bare-metal toolchain, and its driver has no notion of a shared object -
# it drops `-shared` on the floor and links an executable, which
# announces itself as "undefined reference to `main`" from a library that
# has no main and was never supposed to. ld itself is perfectly capable;
# it is the specs in front of it that are not.
$(LIBC_SO): $(LIBC_SO_SRCS) $(UOBJ)/setjmp.o $(UOBJ)/symtab.o
	@mkdir -p $(UOBJ)/pic
	@for src in $(LIBC_SO_SRCS); do \
	  obj=$(UOBJ)/pic/$$(echo $$src | tr / _ | sed 's/\.c$$/.o/'); \
	  $(CC) -std=c11 -O2 -ffreestanding -fno-stack-protector -fPIC \
	    -mcmodel=small -mno-red-zone -ftls-model=initial-exec \
	    -Iuser_space/lib -Iuser_space/libc/include -Isystem_api/include \
	    -c $$src -o $$obj || exit 1; \
	done
	$(LD) -shared -soname libc.so -o $@ $(UOBJ)/pic/*.o \
	  $(UOBJ)/setjmp.o $(UOBJ)/symtab.o

# ---- M99: the dynamic linker, in the sysroot ---------------------------
#
# M95 built ld-lean.so inside tools/build-dynamic.sh with the
# x86_64-lean_os compiler, which was right for M95 and is one milestone
# out of date. Two things changed:
#
#   1. **dlopen lives in here, not in libc.** A dynamic program that
#      calls dlopen has to have the loader on its link line, and M95's
#      answer was to name a path by hand -
#      `"$OUT/ld-lean.so"` on every link - which is precisely the flag
#      invented by hand that M94 exists to abolish. `-lc` finds libc
#      because libc is in the sysroot; the loader has to be there for the
#      same reason, and then LIB_SPEC can add it and nobody passes
#      anything.
#   2. **The ordering is the same one libc.so already has.** The
#      toolchain's own build regenerates the sysroot and links against
#      it, so the sysroot cannot contain a file that only the toolchain
#      can produce. Built with x86_64-elf-gcc, exactly like libc.so and
#      for the argument written above it.
#
# -fvisibility=hidden and -e _start for the reasons ld-start.S gives at
# length: everything in this object is its own, and the four dl* entry
# points are the only things a program may resolve against it.
LD_SO := $(BUILD)/ld-lean.so

$(LD_SO): user_space/ld/ld-lean.c user_space/ld/ld-start.S
	@mkdir -p $(UOBJ)/pic
	$(CC) -c -o $(UOBJ)/pic/ld-start.o user_space/ld/ld-start.S \
	  -Isystem_api/include
	$(CC) -c -o $(UOBJ)/pic/ld-lean.o user_space/ld/ld-lean.c \
	  -std=c11 -O2 -ffreestanding -fno-stack-protector -fPIC \
	  -mcmodel=small -mno-red-zone -fvisibility=hidden \
	  -Wall -Wextra -Isystem_api/include
	$(LD) -shared -soname ld-lean.so -e _start --no-undefined \
	  -o $@ $(UOBJ)/pic/ld-start.o $(UOBJ)/pic/ld-lean.o

sysroot: $(LIBC_A) $(LIBC_SO) $(LD_SO) $(UOBJ)/crt0.o $(UOBJ)/crti.o $(UOBJ)/crtn.o
	@rm -rf $(SYSROOT)
	@mkdir -p $(SYSROOT)/usr/local/include $(SYSROOT)/usr/include $(SYSROOT)/usr/lib
	@cp -R user_space/libc/include/. $(SYSROOT)/usr/local/include/
	@cp -R system_api/include/. $(SYSROOT)/usr/include/
	@cp user_space/lib/syscall_wrappers.h $(SYSROOT)/usr/include/
	@cp $(LIBC_A) $(SYSROOT)/usr/lib/libc.a
	@cp $(LIBC_SO) $(SYSROOT)/usr/lib/libc.so
	@# M99: the loader, so that -pie can find dlopen without a path
	@# being typed. See $(LD_SO)'s rule above.
	@cp $(LD_SO) $(SYSROOT)/usr/lib/ld-lean.so
	@cp $(UOBJ)/crt0.o $(SYSROOT)/usr/lib/crt1.o
	@# M97: and the PIE startup under the name the driver looks for.
	@# crt0-pie.asm is crt0.asm with its two calls routed through the
	@# PLT; every toolchain calls that file Scrt1.o, so this one does
	@# too. It was built by tools/build-dynamic.sh into its own output
	@# directory before, which meant the compiler could not find it -
	@# a `-pie` link had to name it by path. It is a startup file, so
	@# it belongs in the sysroot beside the other three.
	@nasm -f elf64 -o $(UOBJ)/crt0-pie.o user_space/lib/crt0-pie.asm
	@cp $(UOBJ)/crt0-pie.o $(SYSROOT)/usr/lib/Scrt1.o
	@cp $(UOBJ)/crti.o $(SYSROOT)/usr/lib/crti.o
	@cp $(UOBJ)/crtn.o $(SYSROOT)/usr/lib/crtn.o
	@cp user_space/lib/user.ld $(SYSROOT)/usr/lib/lean_os.ld
	@# M97: libm.a, and it is empty on purpose.
	@#
	@# g++'s link spec ends in `-lm` on essentially every target, so a
	@# C++ program does not link at all without a file by that name -
	@# not because it uses libm, but because the driver names it. The
	@# math this libc has lives in libc.a (user_space/libc/src/math.c)
	@# and putting a second copy here would be two definitions of
	@# every symbol the moment a program named both.
	@#
	@# An empty archive is the truthful shape: "there is nothing in
	@# libm that is not already in libc". It is what several small
	@# systems ship for exactly this reason, and the alternative -
	@# teaching the target's LINK_SPEC to drop -lm - would make this
	@# toolchain differ from every other one in a way somebody would
	@# have to rediscover.
	@rm -f $(SYSROOT)/usr/lib/libm.a
	@$(AR) rcs $(SYSROOT)/usr/lib/libm.a 2>/dev/null || true
	@# M99: libdl.a, empty, and for exactly the argument libm.a makes
	@# above. dlopen, dlsym, dlclose and dlerror are in the dynamic
	@# linker - they have to be, because the loader is the only thing
	@# that knows what is loaded - and LIB_SPEC puts it on the line of
	@# every -pie link. But `-ldl` is what a configure script from the
	@# last thirty years writes down when it wants them, and glibc
	@# answers that today with an empty stub for the same reason: the
	@# truthful shape is "there is nothing in libdl that is not already
	@# reachable", not "no such library".
	@rm -f $(SYSROOT)/usr/lib/libdl.a
	@$(AR) rcs $(SYSROOT)/usr/lib/libdl.a 2>/dev/null || true
	@# M100: and the library stack, if it has been built.
	@#
	@# The `rm -rf` at the top of this rule is right and stays: a
	@# generated directory that is patched in place is a directory
	@# nobody can reason about. But M100's libraries are installed INTO
	@# a sysroot by their own `make install`, and the next library in
	@# the dependency order links against the last one - so `make
	@# sysroot`, which tools/gcc-test.sh runs on every invocation, used
	@# to delete zlib out from under libpng between two runs of the same
	@# script.
	@#
	@# tools/build-thirdparty.sh installs each library into
	@# $(BUILD)/thirdparty-sysroot instead, which it owns, and this
	@# copies that tree over the generated one. The generated half is
	@# still generated; the ported half is still a record of what was
	@# installed; and neither one is maintained by hand.
	@if [ -d $(BUILD)/thirdparty-sysroot ]; then \
	  cp -R $(BUILD)/thirdparty-sysroot/. $(SYSROOT)/; \
	  echo "sysroot: + the M100 library stack from $(BUILD)/thirdparty-sysroot"; \
	fi
	@echo "sysroot: $(SYSROOT) - $$(ls $(SYSROOT)/usr/local/include $(SYSROOT)/usr/include | grep -c . ) header entries, libc.a $$(du -h $(LIBC_A) | cut -f1)"

# M93 (second attempt): the same list, for a script that preseeds an image
# that is not $(IMAGE). tools/image-tree-test.sh builds a copy and fills it
# itself, and the order matters for the reason the paragraph above gives -
# so it asks for the list rather than keeping a second one that would be
# right until this line changes.
print-user-programs:
	@echo $(USER_PROGRAMS)

# ---- M101: the kernel's symbol table, as a file on the disk ----------
#
# /bin/profile resolves sampled addresses against this. It is a file
# rather than a table inside the kernel because a table inside the kernel
# changes the addresses it describes - see tools/gen-kernel-syms.sh and
# kernel/profile/sampler.h for the full argument.
#
# Not part of `all`, for one reason and it is not laziness: writing a
# file into a *fresh* image claims the first free inode, and kernel.c's
# M22 self-test asserts that launcher slot 0 is `hello` - the first file
# ever seeded. That is the same trap the `preseed` target above documents
# at length. Depending on $(IMAGE) here means the image exists; running
# this after a boot, or after `make preseed`, means every program's inode
# is already spoken for. `make syms` is a thing you do when you want to
# profile, which is exactly when you will also have booted.
KERNEL_SYMS := $(BUILD)/kernel.syms

$(KERNEL_SYMS): $(KERNEL_ELF) tools/gen-kernel-syms.sh
	@./tools/gen-kernel-syms.sh $(KERNEL_ELF) $@

syms: $(IMAGE) $(KERNEL_SYMS) $(LEANFS_PUT)
	@$(LEANFS_PUT) $(IMAGE) $(KERNEL_SYMS) /etc/kernel.syms
	@echo "kernel.syms -> /etc/kernel.syms in $(IMAGE)"


# ---- Q2: the host test tier -----------------------------------------
#
# Everything above this line builds an operating system for x86_64 and is
# checked by booting it. This builds a handful of that kernel's *units*
# for the machine you are sitting at, and runs them in a fraction of a
# second.
#
# The two tiers answer different questions and neither replaces the
# other. `make test-fast` asks "is this function still correct", including
# on the error paths a booted machine cannot reach without genuinely
# running out of something. tools/run-tests.sh asks "does the machine
# still work", which is the only question that can be answered by a
# machine. See tests/check.h for the longer version of that argument.
#
# -Itests/fakes comes FIRST, so a header placed there shadows the kernel's
# own - which is used exactly once, for lib/spinlock.h, whose real version
# is x86 inline assembly. Every other seam is a fake .c file linked in
# place of the real one, so the code under test is the code that ships.
TEST_BUILD  := $(BUILD)/tests
TEST_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -DLEANOS_HOST_TEST \
               -fno-omit-frame-pointer \
               -Itests -Itests/fakes -Ikernel -Isystem_api/include \
               -Iuser_space/lib

# Sanitizers are on by default and that is a decision, not an oversight.
# The whole reason to run kernel code on a host is to get instruments the
# kernel cannot have: on the real machine a one-byte overrun corrupts a
# neighbour and is found some number of milestones later (see kernel.c's
# M81 stack-guard note for what that costs). Here it is a stack trace.
# TEST_SAN=0 turns them off for a timing run.
ifneq ($(TEST_SAN),0)
TEST_CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
endif

TEST_FAKES := tests/fakes/fake_panic.c tests/fakes/fake_klog.c \
              tests/fakes/fake_spinlock.c tests/fakes/fake_pmm.c \
              tests/fakes/fake_vmm.c tests/fakes/fake_blk.c \
              tests/fakes/fake_rtc.c tests/fakes/fake_net.c \
              tests/fakes/fake_pit.c tests/fakes/fake_socket.c \
              tests/fakes/fake_fwcfg.c \
              tests/fakes/fake_arch.c tests/fakes/fake_kernel_objects.c \
              tests/fakes/fake_user_syscalls.c tests/fakes/fake_user_fs.c

# The kernel sources under test, compiled unmodified.
TEST_KERNEL_SRCS := kernel/lib/libk.c kernel/mm/heap.c kernel/fs/leanfs.c \
                    kernel/net/arp.c kernel/net/ip.c kernel/net/icmp.c \
                    kernel/net/udp.c kernel/net/ethernet.c kernel/net/tcp.c \
                    kernel/dev/fwcfg.c kernel/dev/tty.c kernel/dev/pty.c \
                    kernel/sched/sched.c kernel/fs/flock.c kernel/dev/random.c

# M101: the first user-space source in this tier, and it earns its place
# by the same argument the kernel units do. user_space/lib/symtab.c is a
# pure function over a buffer whose failure mode is a *plausible wrong
# answer* - a profile report attributing every sample to the function
# before the right one - which is exactly the class of bug a booted
# machine cannot see, because there is nothing in there to compare
# against. Kept separate from TEST_KERNEL_SRCS rather than folded in:
# these are two different codebases with two different build flags, and a
# list that stopped saying so would be the first step to compiling kernel
# code with user flags.
TEST_USER_SRCS := user_space/lib/symtab.c \
                  user_space/lib/sha256.c user_space/lib/ospkg.c \
                  user_space/lib/fsutil.c \
                  user_space/libc/src/wchar.c user_space/libc/src/errno.c \
                  user_space/libc/src/fnmatch.c user_space/libc/src/libgen.c \
                  user_space/libc/src/getopt.c

TEST_SRCS := tests/runner.c $(wildcard tests/test_*.c) $(TEST_FAKES) \
             $(TEST_KERNEL_SRCS) $(TEST_USER_SRCS)

TEST_BIN := $(TEST_BUILD)/leanos-tests

$(TEST_BUILD):
	mkdir -p $@

# Q12: the sanitizer setting is part of what this binary *is*, so
# switching it has to rebuild. Without this stamp, `make test-fast
# TEST_SAN=0` (which the mutation harness uses) leaves a non-sanitized
# binary that a later plain `make test-fast` considers up to date - and
# then reports "no ASan findings" about a build ASan was never in.
TEST_SAN_STAMP := $(TEST_BUILD)/.san-$(if $(filter 0,$(TEST_SAN)),off,on)

$(TEST_SAN_STAMP): | $(TEST_BUILD)
	@rm -f $(TEST_BUILD)/.san-* && touch $@

# M93 (second attempt): the kernel's own headers are prerequisites too.
#
# They were not, and the hole is the kind this tier exists to catch rather
# than to have: these tests compile kernel *units*, most of what they
# assert lives in a kernel header, and editing one did not rebuild them.
# `make test-fast` after a header-only change re-ran the previous binary
# and reported a pass for code that no longer existed.
#
# Found by breaking kernel/fs/leanfs_format.h's hash on purpose to check
# that a new test could fail: it did not, until the build directory was
# deleted by hand. A test tier that cannot notice an edit is a slower way
# of writing "PASS".
#
# One `find` over every header rather than -MMD dependency files: this
# recipe compiles all of TEST_SRCS in a single command, so there is no
# per-object .d to include, and the whole binary builds in under a second
# - a rebuild that is occasionally unnecessary costs less than a
# dependency graph that is occasionally wrong.
TEST_HDRS := $(shell find kernel system_api tests -name '*.h' 2>/dev/null)

$(TEST_BIN): $(TEST_SRCS) $(TEST_HDRS) $(TEST_SAN_STAMP) | $(TEST_BUILD)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_SRCS)

# The fast tier. Deliberately does not depend on `all` - the point is that
# it runs without a cross-toolchain, which is what makes it the check worth
# running on every edit.
test-fast: $(TEST_BIN)
	@$(TEST_BIN) $(TEST_FILTER)

# ---- Q8: coverage, measured rather than argued ------------------------
#
# The same move M92 made for the disk and M69 made for latency, applied to
# the tests themselves. This covers only what the host tier builds - the
# units in TEST_KERNEL_SRCS - and says so: it is not a number for the
# whole kernel and must not be quoted as one. The QEMU-only code is
# covered by the boot markers, which is a different instrument and gets a
# different treatment (see Q8 in milestones.md).
#
# `make coverage` prints a per-file percentage and writes an HTML report
# when llvm-cov is available.
COV_BUILD  := $(BUILD)/coverage
# Apple's bundled clang ships neither libFuzzer nor llvm-profdata, so both
# this and the fuzz targets below look for a real LLVM first. Defined here
# because coverage comes first in this file; the fuzz section reuses it.
LLVM_CC    := $(shell for c in /opt/homebrew/opt/llvm/bin/clang \
                               /usr/local/opt/llvm/bin/clang \
                               clang; do \
                        command -v $$c >/dev/null 2>&1 && echo $$c && break; \
                      done)
COV_CC     := $(LLVM_CC)
COV_CFLAGS := -std=c11 -g -O0 -Wall -Wextra \
              -fprofile-instr-generate -fcoverage-mapping \
              -Itests -Itests/fakes -Ikernel -Isystem_api/include

$(COV_BUILD):
	mkdir -p $@

# Q11: the ratchet. `make coverage` reports; `make coverage-check` fails
# a commit that lowers the number.
#
# The floor lives in tests/coverage-floor.tsv and is raised by hand, with
# the commit that raised it recorded - the same discipline
# tests/budgets.tsv uses, and for the same reason: a number that moves on
# its own is a number nobody trusts. Lowering one is allowed and has to
# be deliberate, because deleting a test can be the right call.
COVERAGE_FLOOR := tests/coverage-floor.tsv

coverage-check: coverage
	@python3 tools/coverage-ratchet.py $(COV_BUILD)/report.txt $(COVERAGE_FLOOR)

coverage: | $(COV_BUILD)
	@$(COV_CC) $(COV_CFLAGS) -o $(COV_BUILD)/tests $(TEST_SRCS)
	@cd $(COV_BUILD) && LLVM_PROFILE_FILE=tests.profraw ./tests --slow >/dev/null || true
	@PROFDATA=$$(dirname $(COV_CC))/llvm-profdata; COV=$$(dirname $(COV_CC))/llvm-cov; \
	 if [ ! -x "$$PROFDATA" ]; then PROFDATA=llvm-profdata; COV=llvm-cov; fi; \
	 $$PROFDATA merge -sparse $(COV_BUILD)/tests.profraw -o $(COV_BUILD)/tests.profdata && \
	 $$COV report $(COV_BUILD)/tests -instr-profile=$(COV_BUILD)/tests.profdata \
	   $(TEST_KERNEL_SRCS) $(TEST_USER_SRCS) | tee $(COV_BUILD)/report.txt && \
	 $$COV show $(COV_BUILD)/tests -instr-profile=$(COV_BUILD)/tests.profdata \
	   $(TEST_KERNEL_SRCS) $(TEST_USER_SRCS) -format=html -o $(COV_BUILD)/html >/dev/null && \
	 echo "" && echo "line-by-line report: $(COV_BUILD)/html/index.html"

# ---- Q12: mutation testing -------------------------------------------
#
# Coverage says which lines ran. This says whether anything would have
# noticed if they were wrong - see tools/mutate.py, and Q7 in
# milestones.md for the test that passed against a build with the bug
# still in it, which is why this exists.
#
#   make mutate                        every file the host tier builds
#   make mutate FILE=kernel/mm/heap.c  one of them
#   make mutate MUTANTS=40             a sample rather than the census
#
# MUTANTS samples per file with a fixed seed, so a sampled run is
# repeatable and two runs are comparable.
FILE    ?= $(TEST_KERNEL_SRCS)
MUTANTS ?= 0
mutate:
	@python3 tools/mutate.py $(FILE) --limit $(MUTANTS)

# ---- Q4: fuzzing -----------------------------------------------------
#
# The parsers that read bytes off the wire, and the one that reads a
# superblock somebody else wrote. libFuzzer is a clang flag rather than a
# dependency - clang is already needed for the EFI app - and nothing here
# is linked into anything that ships.
#
# `make fuzz-run` is a 60-second dose per target, which is a smoke test
# rather than a campaign: enough to catch something a change just broke,
# not enough to find something subtle. A real run is
# `build/fuzz/fuzz_net -max_total_time=3600 tests/corpus/net`, and Q10
# schedules one nightly.
#
# A crashing input lands in tests/corpus/<target>/ as crash-<hash>, put
# there by -artifact_prefix rather than dropped in the working directory.
# That matters for two reasons: the repo root stays clean, and - the
# important one - a finding lands somewhere git will show it. The corpus
# itself is ignored and the crash-*/leak-*/timeout-*/oom-* files
# deliberately are not, so a fuzzer that finds something produces exactly
# one new file in `git status` and it is the one worth keeping.
FUZZ_BUILD  := $(BUILD)/fuzz
# Apple's bundled clang does not ship libFuzzer's runtime
# (libclang_rt.fuzzer_osx.a is simply absent), so this looks for a real
# LLVM first and falls back to whatever `clang` is - which works on Linux,
# where the distribution clang has it. `make fuzz` says so plainly rather
# than failing with a linker error about a missing archive.
FUZZ_CC     := $(LLVM_CC)
FUZZ_CFLAGS := -std=c11 -g -O1 -Wall -Wextra \
               -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
               -Itests -Itests/fakes -Ikernel -Isystem_api/include
FUZZ_FAKES  := tests/fakes/fake_panic_abort.c tests/fakes/fake_klog.c \
               tests/fakes/fake_spinlock.c tests/fakes/fake_pmm.c \
               tests/fakes/fake_vmm.c tests/fakes/fake_blk.c \
               tests/fakes/fake_rtc.c tests/fakes/fake_net.c \
               tests/fakes/fake_pit.c tests/fakes/fake_socket.c \
              tests/fakes/fake_fwcfg.c \
              tests/fakes/fake_arch.c tests/fakes/fake_kernel_objects.c
FUZZ_TARGETS := $(FUZZ_BUILD)/fuzz_net $(FUZZ_BUILD)/fuzz_leanfs

$(FUZZ_BUILD):
	mkdir -p $@

$(FUZZ_BUILD)/fuzz_net: tests/fuzz/fuzz_net.c $(FUZZ_FAKES) $(TEST_KERNEL_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_FAKES) $(TEST_KERNEL_SRCS)

$(FUZZ_BUILD)/fuzz_leanfs: tests/fuzz/fuzz_leanfs.c $(FUZZ_FAKES) $(TEST_KERNEL_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_FAKES) $(TEST_KERNEL_SRCS)

fuzz: $(FUZZ_TARGETS)
	@echo "fuzz targets built with $(FUZZ_CC)"

FUZZ_SECONDS ?= 60
fuzz-run: $(FUZZ_TARGETS)
	@for t in $(FUZZ_TARGETS); do \
		name=$$(basename $$t | sed 's/^fuzz_//'); \
		corpus=tests/corpus/$$name; \
		mkdir -p $$corpus; \
		echo "== $$t ($(FUZZ_SECONDS)s, corpus $$corpus)"; \
		$$t -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 \
		   -artifact_prefix=$$corpus/ $$corpus || exit 1; \
	done
	@echo "fuzzing found nothing in $(FUZZ_SECONDS)s per target."

# The two tiers above the fast one. tools/run-tests.sh is the single
# source of truth for what each contains - these are here so that `make
# test` works, because that is what people type.
test:
	@./tools/run-tests.sh

test-full:
	@./tools/run-tests.sh --full

.PHONY: test-fast test test-full test-visual coverage coverage-check fuzz fuzz-run mutate

# Lets tools/build-user-program.sh (and anyone else) read this Makefile's
# own variables - e.g. `make print-USER_CFLAGS` - instead of hardcoding a
# second copy of flags that would silently drift out of sync with the
# ones actually used to build this project's own user-space programs.
print-%:
	@echo $($*)

# The .d files -MMD leaves beside every object (see CFLAGS above). Included
# with a leading `-` so a from-scratch build, where none of them exist yet,
# doesn't warn about every one.
# M94: the object directories only, not all of $(BUILD).
#
# This was `find $(BUILD) -name '*.d'`, which is every dependency file
# this build ever wrote and also every file called *.d that anything else
# put under build/. The toolchain port unpacks binutils and gcc there,
# and binutils' test suite contains files named *.d that are not
# makefiles - `make` read one and stopped with "target pattern contains
# no `%'", which is a confusing way to be told that a wildcard was too
# wide. Named directories rather than a filter, because the fix should
# say what this line is for.
-include $(shell find $(KOBJ) $(UOBJ) $(TEST_BUILD) -name '*.d' 2>/dev/null)

# Q1: everything except the firmware.
#
# `rm -rf $(BUILD)` took build/ovmf with it, which is the one thing under
# build/ that costs several minutes to recreate (tools/build-ovmf.sh
# clones and builds edk2) and the one thing that never changes - it is
# not this project's output, it is a dependency that happens to live
# here. A `make clean` that silently sets up a ten-minute rebuild is a
# `make clean` people avoid running.
#
# `make distclean` is the one that takes it too.
clean:
	rm -rf $(filter-out $(BUILD)/ovmf,$(wildcard $(BUILD)/*))

distclean:
	rm -rf $(BUILD)
