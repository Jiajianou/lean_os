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
          -mno-red-zone -Wall -Wextra -Werror -Ikernel -c

STAGE1_BIN := $(BUILD)/stage1.bin
STAGE2_BIN := $(BUILD)/stage2.bin
KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
KERNEL_SECTORS_FILE := $(BUILD)/kernel.sectors
IMAGE      := $(BUILD)/os-image.bin

KERNEL_C_SRCS := $(shell find kernel -name '*.c' -not -path 'kernel/boot/*')
KERNEL_ASM_SRCS := kernel/arch/x86_64/entry.asm

KERNEL_OBJS := $(KOBJ)/entry.o \
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

$(KOBJ)/entry.o: $(KERNEL_ASM_SRCS) | $(KOBJ)
	$(AS) -f elf64 $< -o $@

$(KOBJ)/%.o: kernel/%.c | $(KOBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

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

$(IMAGE): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN)
	cat $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_BIN) > $(IMAGE)

run: all
	@./tools/run-qemu.sh

clean:
	rm -rf $(BUILD)
