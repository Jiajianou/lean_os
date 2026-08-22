# lean_os top-level build
#
# Toolchain is dev-time only (see docs/toolchain.md) — nothing here ships
# inside the OS image.

AS      := nasm
CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy
QEMU    := qemu-system-x86_64

BUILD := build
BOOT  := kernel/boot
KOBJ  := $(BUILD)/kernel_obj

CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic \
          -mno-red-zone -Wall -Wextra -Werror -Ikernel -Isystem_api/include -c

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
USER_CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-pic \
               -mcmodel=large -mno-red-zone -Wall -Wextra -Werror \
               -Iuser_space/lib -Isystem_api/include -c

STAGE1_BIN := $(BUILD)/stage1.bin
STAGE2_BIN := $(BUILD)/stage2.bin
KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
KERNEL_SECTORS_FILE := $(BUILD)/kernel.sectors
IMAGE      := $(BUILD)/os-image.bin

UOBJ      := $(BUILD)/user_obj
USER_LD   := user_space/lib/user.ld
USER_LIBOBJS := $(UOBJ)/crt0.o $(UOBJ)/syscall_wrappers.o $(UOBJ)/str.o

# Every user program this project ships (M13): coreutils in bin/, plus
# init and shell in their own directories. Each becomes build/NAME.elf,
# linked against USER_LIBOBJS, and all of them get embedded into the
# kernel image together (kernel/proc/embed_programs.asm) - there's still
# no filesystem driver *stage2* can use to load from disk, only the
# kernel's own (M12), so this is still how anything gets onto the disk
# leanfs formats on first boot in the first place.
USER_PROGRAMS := hello echo cat ls init shell
USER_PROGRAM_ELFS := $(foreach p,$(USER_PROGRAMS),$(BUILD)/$(p).elf)

KERNEL_C_SRCS := $(shell find kernel -name '*.c' -not -path 'kernel/boot/*')
KERNEL_ASM_SRCS := $(shell find kernel -name '*.asm' -not -path 'kernel/boot/*')

KERNEL_OBJS := $(patsubst kernel/%.asm,$(KOBJ)/%.o,$(KERNEL_ASM_SRCS)) \
               $(patsubst kernel/%.c,$(KOBJ)/%.o,$(KERNEL_C_SRCS))

.PHONY: all run clean

all: $(IMAGE)

$(BUILD) $(KOBJ):
	mkdir -p $@

$(STAGE1_BIN): $(BOOT)/stage1.asm | $(BUILD)
	$(AS) -f bin $< -o $@

# stage2 needs to know how many sectors to read the kernel image from disk.
# Depending on KERNEL_BIN (rather than KERNEL_SECTORS_FILE directly) is
# deliberate: the sectors file is a side effect of that rule's recipe, not
# a target Make knows how to build on its own, so this is what actually
# guarantees it exists by the time the recipe below runs.
$(STAGE2_BIN): $(BOOT)/stage2.asm $(KERNEL_BIN) | $(BUILD)
	$(AS) -f bin -D KERNEL_SECTOR_COUNT=$$(cat $(KERNEL_SECTORS_FILE)) $< -o $@

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
# guarantee" situation stage2.bin's kernel.sectors dependency is.
$(BUILD)/%.elf: $(UOBJ)/%.o $(USER_LIBOBJS) $(USER_LD)
	$(LD) -T $(USER_LD) -o $@ $(USER_LIBOBJS) $(UOBJ)/$*.o

$(KOBJ)/proc/embed_programs.o: $(USER_PROGRAM_ELFS)

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	$(LD) -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

# Pads kernel.bin up to a whole number of 512-byte sectors (so the image
# layout is exact — no relying on how a short final sector reads off
# disk) and records that sector count for stage2 to read the kernel back
# with.
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -f%z $@); \
	sectors=$$(( (size + 511) / 512 )); \
	padded=$$(( sectors * 512 )); \
	truncate -s $$padded $@; \
	echo $$sectors > $(KERNEL_SECTORS_FILE)

# leanfs (kernel/fs/leanfs.c) starts at sector 2048 (1 MiB) and needs
# 4105 sectors (1 superblock + 7 inode-table + 1 bitmap + 4096 data,
# matching leanfs.c's own layout constants exactly - if those ever
# change, this has to move with them). The boot image (stage1+stage2+
# kernel.bin) has to stay well clear of that, and the disk file itself
# has to actually be big enough to hold the whole filesystem region, or
# QEMU has nothing there for the ATA driver to read/write.
FS_START_LBA     := 2048
FS_TOTAL_SECTORS := 4105
IMAGE_SECTORS    := 8192

$(IMAGE): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	@boot_sectors=$$(( ($$(stat -f%z $(STAGE1_BIN)) + $$(stat -f%z $(STAGE2_BIN)) + $$(stat -f%z $(KERNEL_BIN))) / 512 )); \
	if [ $$boot_sectors -ge $(FS_START_LBA) ]; then \
		echo "error: boot image ($$boot_sectors sectors) has grown into leanfs's start (LBA $(FS_START_LBA)) - move FS_START_LBA out further" >&2; \
		exit 1; \
	fi
	cat $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) > $(IMAGE)
	truncate -s $$(( $(IMAGE_SECTORS) * 512 )) $(IMAGE)

run: all
	@./tools/run-qemu.sh

clean:
	rm -rf $(BUILD)
