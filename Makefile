# lean_os top-level build
#
# Toolchain is dev-time only (see docs/toolchain.md) — nothing here ships
# inside the OS image.

AS    := nasm
CC    := x86_64-elf-gcc
LD    := x86_64-elf-ld
QEMU  := qemu-system-x86_64

BUILD := build
BOOT  := kernel/boot

STAGE1_BIN := $(BUILD)/stage1.bin
STAGE2_BIN := $(BUILD)/stage2.bin
IMAGE      := $(BUILD)/os-image.bin

.PHONY: all run clean

all: $(IMAGE)

$(BUILD):
	mkdir -p $(BUILD)

$(STAGE1_BIN): $(BOOT)/stage1.asm | $(BUILD)
	$(AS) -f bin $< -o $@

$(STAGE2_BIN): $(BOOT)/stage2.asm | $(BUILD)
	$(AS) -f bin $< -o $@

$(IMAGE): $(STAGE1_BIN) $(STAGE2_BIN)
	cat $(STAGE1_BIN) $(STAGE2_BIN) > $(IMAGE)

run: all
	@./tools/run-qemu.sh

clean:
	rm -rf $(BUILD)
