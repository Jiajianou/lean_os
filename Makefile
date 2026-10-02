
ifeq ($(origin AS),default)
AS      := nasm
endif
ifeq ($(origin CC),default)
CC      := x86_64-elf-gcc
endif
ifeq ($(origin LD),default)
LD      := x86_64-elf-ld
AR      := x86_64-elf-ar
endif
OBJCOPY := x86_64-elf-objcopy
NM      := x86_64-elf-nm
QEMU    := qemu-system-x86_64

UEFI_CC      := clang
UEFI_CC_TARGET := x86_64-unknown-windows
UEFI_LINK    := lld-link
MFORMAT      := mformat
MMD          := mmd
MCOPY        := mcopy

BUILD := build
BOOT  := kernel/boot
KOBJ  := $(BUILD)/kernel_obj

HOSTCC       := cc
LEANFS_PUT   := $(BUILD)/leanfs-put

GEN_FONT     := $(BUILD)/gen-font
GEN_ICONS    := $(BUILD)/gen-icons
ICON_STAMP   := $(BUILD)/.icons-checked
FONT_STAMP   := $(BUILD)/.font-check-stamp
FONT_FILES   := kernel/drivers/font8x16.h kernel/drivers/font8x16.c \
                user_space/library/font8x16.h user_space/library/font8x16.c \
                user_space/library/user_interface_font.h user_space/library/user_interface_font.c

CFLAGS := -std=c11 -O1 -ffreestanding -fno-stack-protector -fno-pic \
          -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror \
          -MMD -MP -Ikernel -Isystem_api/include -c

LVGL_INCLUDES := -Ithird_party/lvgl -DLV_CONF_INCLUDE_SIMPLE

USER_CFLAGS := -std=c11 -O1 -ffreestanding -fno-stack-protector -fno-pic \
               -mcmodel=large -mno-red-zone -Wall -Wextra -Werror \
               -ffunction-sections -fdata-sections \
               -MMD -MP -Iuser_space/library -Iuser_space/libc/include \
               -Isystem_api/include $(LVGL_INCLUDES) -c

MBR_BIN    := $(BUILD)/mbr.bin
KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
KERNEL_SECTORS_FILE := $(BUILD)/kernel.sectors
KERNEL_PAGES_FILE   := $(BUILD)/kernel.pages
IMAGE      := $(BUILD)/os-image.bin
UEFI_BOOT_OBJ := $(BUILD)/uefi_boot.obj
UEFI_OPTIONS_OBJ := $(BUILD)/uefi_boot_options.obj
UEFI_BOOT_EFI := $(BUILD)/BOOTX64.EFI

UOBJ      := $(BUILD)/user_obj
USER_LD   := user_space/library/user.ld
USER_LIBOBJS := $(UOBJ)/crt0.o $(UOBJ)/crti.o $(UOBJ)/syscall_wrappers.o $(UOBJ)/string_utilities.o $(UOBJ)/malloc.o \
                $(UOBJ)/graphics.o $(UOBJ)/font8x16.o $(UOBJ)/window_manager_client.o $(UOBJ)/wallpaper.o $(UOBJ)/wallpaper_picture.o $(UOBJ)/bitmap_file.o \
                $(UOBJ)/settings_file.o $(UOBJ)/children.o $(UOBJ)/icons.o $(UOBJ)/icon_draw.o \
                $(UOBJ)/user_interface_font.o $(UOBJ)/recent.o $(UOBJ)/sntp.o $(UOBJ)/dns.o $(UOBJ)/http.o \
                $(UOBJ)/libc_string.o $(UOBJ)/libc_stdlib.o $(UOBJ)/libc_stdio.o \
                $(UOBJ)/libc_math.o $(UOBJ)/libc_math_long_double.o $(UOBJ)/libc_time.o \
                $(UOBJ)/libc_env.o $(UOBJ)/libc_unistd.o \
                $(UOBJ)/libc_signal.o \
                $(UOBJ)/libc_dirent.o $(UOBJ)/libc_stat.o $(UOBJ)/libc_mman.o \
                $(UOBJ)/libc_pthread.o $(UOBJ)/libc_thread_exit.o $(UOBJ)/libc_semaphore.o $(UOBJ)/libc_dlfcn.o $(UOBJ)/libc_nl_types.o $(UOBJ)/libc_locale_functions.o $(UOBJ)/libc_fenv.o $(UOBJ)/libc_strtold.o $(UOBJ)/libc_timer.o $(UOBJ)/libc_stack_protector.o $(UOBJ)/libc_decimal_powers.o $(UOBJ)/libc_sendfile.o $(UOBJ)/libc_errno.o $(UOBJ)/libc_wchar.o $(UOBJ)/libc_locale.o \
                $(UOBJ)/libc_poll.o $(UOBJ)/libc_resource.o \
                $(UOBJ)/libc_statvfs.o $(UOBJ)/libc_utime.o $(UOBJ)/libc_pwd.o \
                $(UOBJ)/libc_termios.o $(UOBJ)/libc_grp.o $(UOBJ)/libc_libgen.o \
                $(UOBJ)/libc_fnmatch.o $(UOBJ)/libc_strings.o $(UOBJ)/libc_sysinfo.o \
                $(UOBJ)/libc_libintl.o \
                $(UOBJ)/libc_regex.o $(UOBJ)/libc_syslog.o \
                $(UOBJ)/libc_socket.o $(UOBJ)/libc_netdb.o \
                $(UOBJ)/libc_resolv.o $(UOBJ)/libc_ifaddrs.o $(UOBJ)/libc_uchar.o \
                $(UOBJ)/libc_wctype.o $(UOBJ)/libc_ctype.o \
                $(UOBJ)/libc_fcntl.o $(UOBJ)/libc_scanf.o $(UOBJ)/libc_mntent.o \
                $(UOBJ)/libc_xattr.o $(UOBJ)/libc_klog.o $(UOBJ)/libc_getopt.o $(UOBJ)/libc_reboot.o $(UOBJ)/libc_tls.o \
                $(UOBJ)/libc_pty.o $(UOBJ)/libc_select.o $(UOBJ)/libc_realpath.o $(UOBJ)/libc_popen.o \
                $(UOBJ)/libc_iconv.o $(UOBJ)/libc_iconv_tables.o $(UOBJ)/libc_wallclock.o \
                $(UOBJ)/libc_readyfds.o \
                $(UOBJ)/sha256.o $(UOBJ)/os_package.o $(UOBJ)/file_system_utilities.o \
                $(UOBJ)/file_browser.o $(UOBJ)/time_format.o \
                $(UOBJ)/setjmp.o $(UOBJ)/symbol_table.o $(UOBJ)/crtn.o

THIRD_PARTY_PROGRAMS := whetstone

USER_PROGRAMS := hello echo cat cp ls audiograb libctest netconf nettime nettest tcptest racetest console nslookup fetch httpd caps captest init sh memtest fonttest compositor wm_demo gui_clock gui_paint desktop_shell desktop_icons gui_terminal text_editor wm_stubborn wm_zorder wm_faulter wm_crash badptr shutdown reboot env envtest sigtest treewalk mmaptest threadtest lazytest vmtest forktest exectest jobtest syscalltest profile proftest oomtest futextest fswriter ptytest exhausttest measure faulttest os pkgtest dirtest browsertest netrecv unixtest epolltest memfdtest posixtest basetest browser popuptest
USER_PROGRAMS += $(THIRD_PARTY_PROGRAMS)

LVGL_PROGRAMS := desktop_applications
USER_PROGRAMS += $(LVGL_PROGRAMS)

USER_PROGRAM_ELFS := $(foreach p,$(USER_PROGRAMS),$(BUILD)/$(p).elf)
LVGL_PROGRAM_ELFS := $(foreach p,$(LVGL_PROGRAMS),$(BUILD)/$(p).elf)

KERNEL_C_SRCS := $(shell find kernel -name '*.c' -not -path 'kernel/boot/uefi/*')
KERNEL_ASM_SRCS := $(shell find kernel -name '*.asm' -not -path 'kernel/boot/*' -not -name 'ap_trampoline.asm')

KERNEL_OBJS := $(patsubst kernel/%.asm,$(KOBJ)/%.o,$(KERNEL_ASM_SRCS)) \
               $(patsubst kernel/%.c,$(KOBJ)/%.o,$(KERNEL_C_SRCS))

.PHONY: all run leanfs-put preseed toybox packages fonts browser browser-if-built netsurf os-pkg sysroot print-user-programs syms font font-check clean distclean

all: $(IMAGE)

$(BUILD) $(KOBJ):
	mkdir -p $@

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

$(UOBJ)/%.o: user_space/library/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

$(UOBJ)/libc_%.o: user_space/libc/src/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

THIRD_PARTY_CFLAGS := $(filter-out -Wall -Wextra -Werror,$(USER_CFLAGS)) -Wno-format-security

$(UOBJ)/whetstone.o: third_party/whetstone/whetstone.c | $(UOBJ)
	$(CC) $(THIRD_PARTY_CFLAGS) $< -o $@

LVGL_DIR  := third_party/lvgl
LVGL_A    := $(BUILD)/liblvgl.a
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c')
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(UOBJ)/lvgl/%.o,$(LVGL_SRCS))
LVGL_CFLAGS := $(THIRD_PARTY_CFLAGS) $(LVGL_INCLUDES)

$(UOBJ)/lvgl/%.o: $(LVGL_DIR)/%.c | $(UOBJ)
	@mkdir -p $(dir $@)
	$(if $(V),,@echo "  CC      $@")
	$(if $(V),,@)$(CC) $(LVGL_CFLAGS) $< -o $@

$(LVGL_A): $(LVGL_OBJS)
	$(if $(V),,@echo "  AR      $@")
	$(if $(V),,@)$(AR) rcs $@ $(LVGL_OBJS)

$(UOBJ)/%.o: user_space/library/%.asm | $(UOBJ)
	$(AS) -f elf64 $< -o $@

$(UOBJ)/%.o: user_space/binaries/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

$(UOBJ)/%.o: user_space/init/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

$(UOBJ)/%.o: user_space/shell/%.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

# The program is /bin/sh and its source is shell.c: the cleanup that renamed
# the file left nothing that builds sh.o, so every tree since reused whatever
# build/sh.elf it already had and a fresh checkout could not build at all.
$(UOBJ)/sh.o: user_space/shell/shell.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $@

.SECONDARY:

$(BUILD)/%.elf: $(UOBJ)/%.o $(USER_LIBOBJS) $(USER_LD)
	$(if $(V),,@echo "  LD      $@")
	$(if $(V),,@)$(LD) --gc-sections -T $(USER_LD) -o $@ $(USER_LIBOBJS) $(UOBJ)/$*.o

LVGL_PORT_OBJS := $(UOBJ)/lvgl_leanos.o $(UOBJ)/lvgl_keys.o $(UOBJ)/lvgl_pointer_queue.o $(UOBJ)/lvgl_theme.o \
                  $(UOBJ)/desktop_palette.o $(UOBJ)/application_dispatch.o

DESKTOP_APPLICATION_OBJS := $(UOBJ)/desktop_application_settings.o \
                            $(UOBJ)/desktop_application_task_manager.o \
                            $(UOBJ)/desktop_application_widgets.o \
                            $(UOBJ)/desktop_application_wireless.o \
                            $(UOBJ)/desktop_application_files.o \
                            $(UOBJ)/desktop_application_wallpaper.o

# Not in USER_PROGRAMS: the reference table is 12,840 values and the kernel
# incbins every embedded program, so this one is installed onto the image by
# tools/math-long-double-test.sh the way /bin/clangtest and /bin/ruststd are.
.PHONY: mathltest
mathltest: $(BUILD)/mathltest.elf

# The reference table is a separate translation unit on purpose: it holds
# thousands of __builtin_ calls GCC folds with MPFR, and the test proves the
# folding by requiring that object to reference no math symbol. Compiling it
# beside the grader, which calls every one of those functions for real, would
# put both in the same object and make that proof unavailable.
$(BUILD)/mathltest.elf: $(UOBJ)/mathltest.o $(UOBJ)/math_long_double_table.o $(USER_LIBOBJS) $(USER_LD)
	$(if $(V),,@echo "  LD      $@")
	$(if $(V),,@)$(LD) --gc-sections -T $(USER_LD) -o $@ $(USER_LIBOBJS) $(UOBJ)/math_long_double_table.o $(UOBJ)/mathltest.o

$(LVGL_PROGRAM_ELFS): $(BUILD)/%.elf: $(UOBJ)/%.o $(USER_LIBOBJS) $(LVGL_PORT_OBJS) $(DESKTOP_APPLICATION_OBJS) $(LVGL_A) $(USER_LD)
	$(if $(V),,@echo "  LD      $@")
	$(if $(V),,@)$(LD) --gc-sections -T $(USER_LD) -o $@ $(USER_LIBOBJS) $(LVGL_PORT_OBJS) $(DESKTOP_APPLICATION_OBJS) $(UOBJ)/$*.o $(LVGL_A)

AP_TRAMPOLINE_BIN := $(BUILD)/ap_trampoline.bin

$(AP_TRAMPOLINE_BIN): kernel/architecture/x86_64/ap_trampoline.asm | $(BUILD)
	$(AS) -f bin $< -o $@

$(KOBJ)/process/embed_ap_trampoline.o: $(AP_TRAMPOLINE_BIN)

$(KOBJ)/process/embed_programs.o: $(USER_PROGRAM_ELFS) | check-embedded-programs

.PHONY: check-embedded-programs
check-embedded-programs:
	@for p in $(USER_PROGRAMS); do \
	  grep -q "^$${p}_elf_start:" kernel/process/embed_programs.asm || { \
	    echo "Makefile: '$$p' is in USER_PROGRAMS but has no incbin block in kernel/process/embed_programs.asm." >&2; \
	    echo "          Add one (see that file's header comment on why it is written out longhand)." >&2; \
	    exit 1; }; \
	done

$(KERNEL_ELF): $(KERNEL_OBJS) kernel/linker.ld
	$(if $(V),,@echo "  LD      $@")
	$(if $(V),,@)$(LD) -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

$(UEFI_BOOT_OBJ): kernel/boot/uefi/boot.c kernel/boot/uefi/efi.h kernel/boot/uefi/efi_proto.h kernel/boot/boot_options.h $(KERNEL_BIN) | $(BUILD)
	$(UEFI_CC) -target $(UEFI_CC_TARGET) -ffreestanding -fshort-wchar -mno-red-zone \
	           -fno-stack-protector -std=c11 -Wall -Wextra -Werror \
	           -DKERNEL_SECTOR_COUNT=$$(cat $(KERNEL_SECTORS_FILE)) \
	           -DKERNEL_IMAGE_PAGES=$$(cat $(KERNEL_PAGES_FILE)) \
	           -Ikernel -Ikernel/boot/uefi -c kernel/boot/uefi/boot.c -o $@

$(UEFI_OPTIONS_OBJ): kernel/boot/boot_options.c kernel/boot/boot_options.h | $(BUILD)
	$(UEFI_CC) -target $(UEFI_CC_TARGET) -ffreestanding -fshort-wchar -mno-red-zone \
	           -fno-stack-protector -std=c11 -Wall -Wextra -Werror \
	           -Ikernel -c kernel/boot/boot_options.c -o $@

$(UEFI_BOOT_EFI): $(UEFI_BOOT_OBJ) $(UEFI_OPTIONS_OBJ)
	$(UEFI_LINK) /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:X64 \
	             /dll /dynamicbase:no /out:$@ $(UEFI_BOOT_OBJ) $(UEFI_OPTIONS_OBJ)

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

# Where the filesystem starts, which is also where the boot image must stop.
# 8192 until M165, when kernel.c's self-test battery grew the boot image to
# 8336 sectors and the check below caught it - doing exactly what it was
# written to do, and naming the two places that have to move together.
#
# The three expressions under it used to repeat the number rather than name
# it, so moving it meant finding all four. They name it now.
#
# It must stay a multiple of LEANFS_SECTORS_PER_BLOCK; tests/test_leanfs_format.c
# has a _Static_assert that says so.
FS_START_LBA     := 16384
FS_INODE_BLOCKS  := 4096
FS_BITMAP_BLOCKS := 16
FS_DATA_BLOCKS   := 524288
FS_TOTAL_SECTORS := $(shell echo $$(( (1 + $(FS_INODE_BLOCKS) + $(FS_BITMAP_BLOCKS) + $(FS_DATA_BLOCKS)) * 8 )))

# 32 MiB rather than the 512 KiB BOOTX64.EFI needs, because the ESP is the one
# partition every other operating system can open: tools/make-hardware-image.sh
# puts a 24 MiB \LOGS\LEANOS.LOG in it that the kernel writes its log into
# (M189), so a machine with no serial port can be read on the next computer.
# M194: the journal sits between the filesystem and the ESP - LEANFS_JOURNAL_BLOCKS
# blocks of it, in kernel/file_system/leanfs_format.h, which is what the kernel
# computes its start from.
FS_JOURNAL_BLOCKS  := 8192
FS_JOURNAL_SECTORS := $(shell echo $$(( $(FS_JOURNAL_BLOCKS) * 8 )))
ESP_START_LBA    := $(shell echo $$(( $(FS_START_LBA) + $(FS_TOTAL_SECTORS) + $(FS_JOURNAL_SECTORS) )))
ESP_SECTOR_COUNT := 65536
IMAGE_SECTORS    := $(shell echo $$(( $(ESP_START_LBA) + $(ESP_SECTOR_COUNT) + 1024 )))

$(IMAGE): $(MBR_BIN) $(KERNEL_BIN) $(UEFI_BOOT_EFI)
	@boot_sectors=$$(( ($$(stat -f%z $(MBR_BIN)) + $$(stat -f%z $(KERNEL_BIN))) / 512 )); \
	if [ $$boot_sectors -ge $(FS_START_LBA) ]; then \
		echo "error: boot image ($$boot_sectors sectors) has grown into the filesystem's start (LBA $(FS_START_LBA)) - move FS_START_LBA out further here and in kernel/file_system/leanfs.c's LEANFS_START_LBA" >&2; \
		exit 1; \
	fi; \
	journal_blocks=$$(awk '$$1 == "#define" && $$2 == "LEANFS_JOURNAL_BLOCKS" { sub(/u$$/, "", $$3); print $$3 }' kernel/file_system/leanfs_format.h); \
	if [ "$$journal_blocks" != "$(FS_JOURNAL_BLOCKS)" ]; then \
		echo "error: FS_JOURNAL_BLOCKS here is $(FS_JOURNAL_BLOCKS) and LEANFS_JOURNAL_BLOCKS in leanfs_format.h is $$journal_blocks - the kernel would journal into the ESP." >&2; \
		exit 1; \
	fi; \
	fs_end=$$(( $(FS_START_LBA) + $(FS_TOTAL_SECTORS) + $(FS_JOURNAL_SECTORS) )); \
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

run:
	@./tools/run-qemu.sh

# -MMD, because this tool reads the on-disk format out of the kernel's own
# headers and the rule used to name only the .c. A change to leanfs.h or
# leanfs_format.h then left a stale tool writing at the old place, which is
# what M165 hit when it moved FS_START_LBA: the tool refused rather than
# corrupting anything - its magic check is written for exactly this - but the
# build should not have handed it a reason to.
$(LEANFS_PUT): tools/leanfs-put.c | $(BUILD)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -MMD -MP -MF $(BUILD)/leanfs-put.d -o $@ $<

leanfs-put: $(LEANFS_PUT)

OS_PKG      := $(BUILD)/os-pkg
OS_PKG_SRCS := tools/os-pkg.c user_space/library/os_package.c user_space/library/sha256.c

$(OS_PKG): $(OS_PKG_SRCS) user_space/library/os_package.h user_space/library/sha256.h \
           system_api/include/capabilities.h | $(BUILD)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Iuser_space/library \
	          -Isystem_api/include -o $@ $(OS_PKG_SRCS)

os-pkg: $(OS_PKG)

$(GEN_ICONS): tools/gen-icons.c | $(BUILD)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -o $@ $< -lm

icons: $(GEN_ICONS)
	@$(GEN_ICONS) --write

icons-check: $(GEN_ICONS)
	@$(GEN_ICONS) --check

$(ICON_STAMP): tools/gen-icons.c user_space/library/icons.c user_space/library/icons.h $(GEN_ICONS) | $(BUILD)
	@$(GEN_ICONS) --check
	@touch $@

$(UOBJ)/icons.o: $(ICON_STAMP)

$(GEN_FONT): tools/gen-font.c | $(BUILD)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -o $@ $<

font: $(GEN_FONT)
	@$(GEN_FONT) --write

font-check: $(GEN_FONT)
	@$(GEN_FONT) --check

$(FONT_STAMP): tools/gen-font.c $(FONT_FILES) $(GEN_FONT) | $(BUILD)
	@$(GEN_FONT) --check
	@touch $@

$(KOBJ)/drivers/font8x16.o: $(FONT_STAMP)
$(UOBJ)/font8x16.o: $(FONT_STAMP)
$(UOBJ)/user_interface_font.o: $(FONT_STAMP)

preseed: $(IMAGE) $(LEANFS_PUT)
	@for p in $(USER_PROGRAMS); do \
		$(LEANFS_PUT) $(IMAGE) $(BUILD)/$$p.elf /bin/$$p; \
	done

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

NETSURF_BIN := $(BUILD)/netsurf/netsurf

$(NETSURF_BIN): tools/build-netsurf.sh user_space/binaries/nsfb_leanos.c
	@./tools/build-netsurf.sh

# The fonts, and the file fontconfig looks for. Their own step rather than
# part of the browser's, because they are a property of the machine: the
# next program that draws text asks the same question through the same
# library, and an image with no /etc/fonts/fonts.conf has no fonts at all as
# far as that library is concerned.
.PHONY: fonts
fonts: $(IMAGE) $(LEANFS_PUT)
	@./tools/install-fonts.sh

# The browser is Chromium's own content_shell since M171, built by
# tools/build-chromium.sh out of Chromium's own ninja - which is not a
# Makefile step, being most of an hour and a 54 GB checkout. `make browser`
# installs what that produced; `browser-if-built` is what the harnesses run
# after a kernel rebuild has recreated the disk, and says so when there is
# nothing to install. NetSurf is still buildable as `make netsurf` and is no
# longer the program the Browser icon opens.
CHROMIUM_SHELL_BIN := $(BUILD)/chromium/src/out/LeanOS/content_shell

browser: $(IMAGE) $(LEANFS_PUT) preseed
	@if [ ! -f $(CHROMIUM_SHELL_BIN) ]; then \
		echo "browser: no $(CHROMIUM_SHELL_BIN) - tools/build-chromium.sh content/shell:content_shell builds it" >&2; \
		exit 1; \
	fi
	@./tools/install-browser.sh

browser-if-built: $(IMAGE) $(LEANFS_PUT)
	@if [ ! -f $(CHROMIUM_SHELL_BIN) ]; then \
		echo "browser: not built - tools/build-chromium.sh content/shell:content_shell, then \`make browser\` (an image without a browser is a valid image)."; \
	else \
		$(MAKE) --no-print-directory preseed >/dev/null || exit 1; \
		./tools/install-browser.sh || exit 1; \
	fi

netsurf: $(NETSURF_BIN) $(IMAGE) $(LEANFS_PUT) preseed fonts
	@./tools/install-netsurf.sh

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

SYSROOT := $(BUILD)/sysroot
LIBC_A  := $(BUILD)/libc.a

LIBC_A_OBJS := $(filter-out $(UOBJ)/crt0.o $(UOBJ)/crti.o $(UOBJ)/crtn.o,$(USER_LIBOBJS))

$(LIBC_A): $(LIBC_A_OBJS)
	@rm -f $@
	$(if $(V),,@echo "  AR      $@")
	$(if $(V),,@)$(AR) rcs $@ $(LIBC_A_OBJS)

all: $(LIBC_A)

$(NETSURF_BIN): $(LIBC_A)

# libdl.a - dlopen and its three companions for a STATICALLY linked program,
# which has no dynamic loader behind it to answer them. Deliberately its own
# archive: a definition in libc.a or libc.so would be bound ahead of the
# loader's real ones and take dynamic loading away from the programs that
# have it. Its source lives outside user_space/libc/src for the same reason -
# that directory is a wildcard into libc.so.
LIBDL_A := $(BUILD)/libdl.a

$(LIBDL_A): user_space/libc/libdl/dlfcn_static.c | $(UOBJ)
	$(CC) $(USER_CFLAGS) $< -o $(UOBJ)/libdl_dlfcn_static.o
	$(AR) rcs $@ $(UOBJ)/libdl_dlfcn_static.o

LIBC_SO := $(BUILD)/libc.so
LIBC_SO_SRCS := $(wildcard user_space/libc/src/*.c) \
                user_space/library/syscall_wrappers.c user_space/library/string_utilities.c \
                user_space/library/malloc.c user_space/library/dns.c

$(LIBC_SO): $(LIBC_SO_SRCS) $(UOBJ)/setjmp.o $(UOBJ)/symbol_table.o
	@mkdir -p $(UOBJ)/pic
	@for src in $(LIBC_SO_SRCS); do \
	  obj=$(UOBJ)/pic/$$(echo $$src | tr / _ | sed 's/\.c$$/.o/'); \
	  $(CC) -std=c11 -O2 -ffreestanding -fno-stack-protector -fPIC \
	    -mcmodel=small -mno-red-zone -ftls-model=initial-exec \
	    -Iuser_space/library -Iuser_space/libc/include -Isystem_api/include \
	    -c $$src -o $$obj || exit 1; \
	done
	$(LD) -shared -soname libc.so -o $@ $(UOBJ)/pic/*.o \
	  $(UOBJ)/setjmp.o $(UOBJ)/symbol_table.o

LD_SO := $(BUILD)/ld-lean.so

$(LD_SO): user_space/loader/ld-lean.c user_space/loader/ld-start.S
	@mkdir -p $(UOBJ)/pic
	$(CC) -c -o $(UOBJ)/pic/ld-start.o user_space/loader/ld-start.S \
	  -Isystem_api/include
	$(CC) -c -o $(UOBJ)/pic/ld-lean.o user_space/loader/ld-lean.c \
	  -std=c11 -O2 -ffreestanding -fno-stack-protector -fPIC \
	  -mcmodel=small -mno-red-zone -fvisibility=hidden \
	  -Wall -Wextra -Isystem_api/include
	$(LD) -shared -soname ld-lean.so -e _start --no-undefined \
	  -o $@ $(UOBJ)/pic/ld-start.o $(UOBJ)/pic/ld-lean.o

# M139: the headers alone, over an existing sysroot. `sysroot` below begins
# with `rm -rf`, which is right for a generated directory but takes the
# fifteen third-party libraries installed into it with it - so a header added
# to user_space/libc/include had no safe way to reach a cross compile short
# of rebuilding the whole library stack. This is that way.
.PHONY: sysroot-headers
sysroot-headers:
	@if [ ! -d $(SYSROOT)/usr/include ]; then \
	  echo "sysroot-headers: no sysroot yet - run 'make sysroot'"; exit 1; fi
	@# Only the headers whose CONTENTS moved, because a copy that rewrites
	@# every mtime is an hour of Chromium rebuilding things nobody touched -
	@# see tools/copy-changed.sh, which is M169 paying for that twice.
	@echo "sysroot-headers: libc  $$(./tools/copy-changed.sh user_space/libc/include $(SYSROOT)/usr/local/include) file(s) changed"
	@echo "sysroot-headers: abi   $$(./tools/copy-changed.sh system_api/include $(SYSROOT)/usr/include) file(s) changed"
	@cmp -s user_space/library/syscall_wrappers.h $(SYSROOT)/usr/include/syscall_wrappers.h || \
	  cp user_space/library/syscall_wrappers.h $(SYSROOT)/usr/include/
	@cmp -s user_space/library/window_manager_client.h $(SYSROOT)/usr/include/window_manager_client.h || \
	  cp user_space/library/window_manager_client.h $(SYSROOT)/usr/include/
	@# And the two it includes, which an ozone platform compiled inside
	@# Chromium's build reaches through it (M171).
	@cmp -s user_space/library/graphics.h $(SYSROOT)/usr/include/graphics.h || \
	  cp user_space/library/graphics.h $(SYSROOT)/usr/include/
	@cmp -s user_space/library/user_interface_font.h $(SYSROOT)/usr/include/user_interface_font.h || \
	  cp user_space/library/user_interface_font.h $(SYSROOT)/usr/include/

# M140: and the library half, for the same reason. M139 could add a header
# without rebuilding the world; adding a libc *function* still could not,
# because the archive a cross compile links against lives in the same
# directory `sysroot` deletes. pthread_atfork was the one that made this
# necessary.
.PHONY: sysroot-libc
sysroot-libc: $(LIBC_A) $(LIBC_SO) $(LD_SO) $(LIBDL_A) $(UOBJ)/crt0.o $(UOBJ)/crti.o $(UOBJ)/crtn.o
	@if [ ! -d $(SYSROOT)/usr/lib ]; then \
	  echo "sysroot-libc: no sysroot yet - run 'make sysroot'"; exit 1; fi
	@cp $(LIBC_A) $(SYSROOT)/usr/lib/libc.a
	@cp $(LIBC_SO) $(SYSROOT)/usr/lib/libc.so
	@cp $(LD_SO) $(SYSROOT)/usr/lib/ld-lean.so
	@cp $(LIBDL_A) $(SYSROOT)/usr/lib/libdl.a
	@cp $(UOBJ)/crt0.o $(SYSROOT)/usr/lib/crt1.o
	@nasm -f elf64 -o $(UOBJ)/crt0-pie.o user_space/library/crt0-pie.asm
	@cp $(UOBJ)/crt0-pie.o $(SYSROOT)/usr/lib/Scrt1.o
	@cp $(UOBJ)/crti.o $(SYSROOT)/usr/lib/crti.o
	@cp $(UOBJ)/crtn.o $(SYSROOT)/usr/lib/crtn.o
	@cp user_space/library/user.ld $(SYSROOT)/usr/lib/lean_os.ld
	@# The empty archives `sysroot` makes, in case this rule is the first
	@# thing run against a sysroot that predates one of them.
	@for stub in libm librt libpthread libresolv libuuid; do \
	  if [ ! -f $(SYSROOT)/usr/lib/$$stub.a ]; then \
	    $(AR) rcs $(SYSROOT)/usr/lib/$$stub.a 2>/dev/null || true; \
	  fi; \
	done
	@echo "sysroot-libc: $(SYSROOT) libraries refreshed from the tree"

# Non-destructive, and it copies only what CHANGED. It used to begin with
# rm -rf and cp -R, which rewrote the modification time of every header in
# the sysroot every time it ran - and gcc-test.sh, cxx-test.sh,
# clang-test.sh and build-dynamic.sh each run it, so every default tier did
# it four times. Chromium's build watches those headers (M169 measured it:
# an hour of libc++, V8, ANGLE and Blink rebuilding for 92 headers nobody
# had changed), which made the tier that grades a milestone the thing that
# cost the next one an hour before it started. copy-changed.sh compares
# contents; a header that did not change keeps its mtime and nothing above
# it rebuilds. The third-party libraries installed here by their own `make
# install` are no longer deleted either, which CLAUDE.md warned about.
define copy_if_changed
cmp -s $(1) $(2) || cp $(1) $(2)
endef

sysroot: $(LIBC_A) $(LIBC_SO) $(LD_SO) $(LIBDL_A) $(UOBJ)/crt0.o $(UOBJ)/crti.o $(UOBJ)/crtn.o
	@mkdir -p $(SYSROOT)/usr/local/include $(SYSROOT)/usr/include $(SYSROOT)/usr/lib
	@./tools/copy-changed.sh user_space/libc/include $(SYSROOT)/usr/local/include >/dev/null
	@./tools/copy-changed.sh system_api/include $(SYSROOT)/usr/include >/dev/null
	@$(call copy_if_changed,user_space/library/syscall_wrappers.h,$(SYSROOT)/usr/include/syscall_wrappers.h)
	@$(call copy_if_changed,user_space/library/window_manager_client.h,$(SYSROOT)/usr/include/window_manager_client.h)
	@$(call copy_if_changed,user_space/library/graphics.h,$(SYSROOT)/usr/include/graphics.h)
	@$(call copy_if_changed,user_space/library/user_interface_font.h,$(SYSROOT)/usr/include/user_interface_font.h)
	@$(call copy_if_changed,$(LIBC_A),$(SYSROOT)/usr/lib/libc.a)
	@$(call copy_if_changed,$(LIBC_SO),$(SYSROOT)/usr/lib/libc.so)
	@# M99: the loader, so that -pie can find dlopen without a path
	@# being typed. See $(LD_SO)'s rule above.
	@$(call copy_if_changed,$(LD_SO),$(SYSROOT)/usr/lib/ld-lean.so)
	@$(call copy_if_changed,$(UOBJ)/crt0.o,$(SYSROOT)/usr/lib/crt1.o)
	@# M97: and the PIE startup under the name the driver looks for.
	@# crt0-pie.asm is crt0.asm with its two calls routed through the
	@# PLT; every toolchain calls that file Scrt1.o, so this one does
	@# too. It was built by tools/build-dynamic.sh into its own output
	@# directory before, which meant the compiler could not find it -
	@# a `-pie` link had to name it by path. It is a startup file, so
	@# it belongs in the sysroot beside the other three.
	@nasm -f elf64 -o $(UOBJ)/crt0-pie.o user_space/library/crt0-pie.asm
	@cp $(UOBJ)/crt0-pie.o $(SYSROOT)/usr/lib/Scrt1.o
	@$(call copy_if_changed,$(UOBJ)/crti.o,$(SYSROOT)/usr/lib/crti.o)
	@$(call copy_if_changed,$(UOBJ)/crtn.o,$(SYSROOT)/usr/lib/crtn.o)
	@$(call copy_if_changed,user_space/library/user.ld,$(SYSROOT)/usr/lib/lean_os.ld)
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
	@# M99 made libdl.a empty for exactly the argument libm.a makes
	@# above: dlopen, dlsym, dlclose and dlerror are in the dynamic
	@# linker - they have to be, because the loader is the only thing
	@# that knows what is loaded - and LIB_SPEC puts it on the line of
	@# every -pie link, so there was nothing in libdl that was not
	@# already reachable.
	@#
	@# M164: that is true of a DYNAMICALLY linked program and of no
	@# other kind. A static one has no loader behind it, so the four
	@# were reachable from nowhere and an empty archive answered `-ldl`
	@# with a link error naming four functions this system does have.
	@# $(LIBDL_A) is the four that truthfully refuse; they are not in
	@# libc.a because a definition there would be bound at link time
	@# and outrank the loader's real ones. See
	@# user_space/libc/libdl/dlfcn_static.c.
	@$(call copy_if_changed,$(LIBDL_A),$(SYSROOT)/usr/lib/libdl.a)
	@# M138: librt.a and libpthread.a, empty, and for the third time the
	@# argument libm.a makes above. Rust's standard library writes
	@# #[link(name = "rt")] and #[link(name = "pthread")] for every unix,
	@# because that is where glibc kept clock_gettime and the pthread
	@# functions until it folded them into libc. This libc never split
	@# them out - sys_clock_gettime and user_space/libc/src/pthread.c are
	@# in libc.a - so the truthful shape is again an empty archive rather
	@# than a second copy of every symbol or a fork of std's link
	@# attributes.
	@rm -f $(SYSROOT)/usr/lib/librt.a $(SYSROOT)/usr/lib/libpthread.a
	@$(AR) rcs $(SYSROOT)/usr/lib/librt.a 2>/dev/null || true
	@$(AR) rcs $(SYSROOT)/usr/lib/libpthread.a 2>/dev/null || true
	@# M150: libresolv.a, empty, for the fourth time. Chromium's //net
	@# writes `-lresolv` because that is where glibc kept res_ninit(3)
	@# until 2.34 folded it into libc; this libc never split it out
	@# (user_space/libc/src/resolv.c is in libc.a), and musl and modern
	@# glibc both answer -lresolv with a stub for the same reason.
	@rm -f $(SYSROOT)/usr/lib/libresolv.a
	@$(AR) rcs $(SYSROOT)/usr/lib/libresolv.a 2>/dev/null || true
	@# M157: libuuid.a, empty, for the fifth time - and this one is empty
	@# for a different reason than the four above, which is worth knowing.
	@# The others are libraries glibc split out and this libc never did.
	@# Chromium's third_party/fontconfig/BUILD.gn writes
	@# `if (!is_win) { libs = ["uuid"] }`, which claims every platform that
	@# is not Windows ships util-linux's libuuid - and fontconfig 2.18 as
	@# Chromium builds it references NOT ONE uuid symbol. That was measured
	@# rather than assumed: x86_64-lean_os-nm -u over all 58 fontconfig
	@# objects reports no uuid_* at all. So the archive is empty because
	@# nothing wants anything from it, and if that ever stops being true
	@# the link fails naming the function, which is the honest failure.
	@rm -f $(SYSROOT)/usr/lib/libuuid.a
	@$(AR) rcs $(SYSROOT)/usr/lib/libuuid.a 2>/dev/null || true
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

print-user-programs:
	@echo $(USER_PROGRAMS)

KERNEL_SYMS := $(BUILD)/kernel.syms

$(KERNEL_SYMS): $(KERNEL_ELF) tools/gen-kernel-syms.sh
	@./tools/gen-kernel-syms.sh $(KERNEL_ELF) $@

syms: $(IMAGE) $(KERNEL_SYMS) $(LEANFS_PUT)
	@$(LEANFS_PUT) $(IMAGE) $(KERNEL_SYMS) /etc/kernel.syms
	@echo "kernel.syms -> /etc/kernel.syms in $(IMAGE)"

TEST_BUILD  := $(BUILD)/tests
TEST_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -DLEANOS_HOST_TEST \
               -fno-omit-frame-pointer \
               -Itests -Itests/fakes -Ikernel -Isystem_api/include \
               -Iuser_space/library $(LVGL_INCLUDES) \
               -idirafter user_space/libc/include
# -idirafter rather than -I, and the distinction is the whole point: this
# libc's headers go AFTER the host's, so every standard name a host test
# compiles against is still the host's own. It reaches only the names macOS
# does not have at all - <libintl.h> is the first - which is what lets a
# function be graded here without its header shadowing forty others.

ifneq ($(TEST_SAN),0)
TEST_CFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
endif

TEST_FAKES := tests/fakes/fake_panic.c tests/fakes/fake_klog.c \
              tests/fakes/fake_spinlock.c tests/fakes/fake_pmm.c \
              tests/fakes/fake_vmm.c tests/fakes/fake_blk.c \
              tests/fakes/fake_rtc.c tests/fakes/fake_net.c \
              tests/fakes/fake_pit.c tests/fakes/fake_socket.c \
              tests/fakes/fake_fwcfg.c tests/fakes/fake_pci.c \
              tests/fakes/fake_arch.c tests/fakes/fake_kernel_objects.c \
              tests/fakes/fake_user_syscalls.c tests/fakes/fake_user_fs.c \
              tests/fakes/fake_user_net.c tests/fakes/fake_framebuffer.c \
              tests/fakes/fake_wireless_glue.c

TEST_KERNEL_SRCS := kernel/library/kernel_library.c kernel/memory_management/heap.c kernel/file_system/leanfs.c \
                    kernel/network/arp.c kernel/network/ip.c kernel/network/icmp.c \
                    kernel/network/udp.c kernel/network/ethernet.c kernel/network/tcp.c kernel/network/ieee80211.c \
                    kernel/network/wpa_crypto.c kernel/network/wpa_handshake.c \
                    kernel/network/wireless_manager.c kernel/network/wireless_simulator.c \
                    kernel/device/fwcfg.c kernel/device/tty.c kernel/device/pty.c \
                    kernel/scheduler/scheduler.c kernel/file_system/flock.c kernel/device/random.c \
                    kernel/drivers/rtl8139_ring.c kernel/inter_process_communication/unix_socket.c \
                    kernel/drivers/usb_hid.c kernel/drivers/xhci_ring.c \
                    kernel/drivers/nvme_split.c kernel/drivers/pci.c \
                    kernel/inter_process_communication/eventfd.c kernel/inter_process_communication/timerfd.c kernel/inter_process_communication/epoll.c \
                    kernel/inter_process_communication/memfd.c \
                    kernel/process/resource_limits.c kernel/boot/boot_options.c \
                    kernel/drivers/hid_report.c kernel/drivers/i2c_hid.c kernel/drivers/touchpad_gestures.c \
                    kernel/drivers/designware_i2c_timing.c kernel/drivers/designware_i2c.c kernel/drivers/intel_wireless_firmware.c kernel/drivers/intel_wireless_transport.c kernel/drivers/intel_wireless.c \
                    kernel/drivers/usb_storage_protocol.c kernel/drivers/disk_log_area.c \
                    kernel/drivers/console.c

TEST_USER_SRCS := user_space/library/symbol_table.c \
                  user_space/library/sha256.c user_space/library/os_package.c \
                  user_space/library/file_system_utilities.c user_space/library/file_browser.c user_space/library/time_format.c \
                  user_space/library/dns.c \
                  user_space/library/graphics.c user_space/library/font8x16.c \
                  user_space/library/user_interface_font.c \
                  user_space/library/icons.c user_space/library/icon_draw.c \
                  user_space/library/wallpaper.c user_space/library/wallpaper_picture.c \
                  user_space/library/bitmap_file.c \
                  user_space/libc/src/wchar.c user_space/libc/src/errno.c \
                  user_space/libc/src/libintl.c \
                  user_space/libc/src/fnmatch.c user_space/libc/src/libgen.c \
                  user_space/libc/src/getopt.c user_space/libc/src/wallclock.c

TEST_SRCS := tests/runner.c $(wildcard tests/test_*.c) $(TEST_FAKES) \
             $(TEST_KERNEL_SRCS) $(TEST_USER_SRCS)

TEST_BIN := $(TEST_BUILD)/leanos-tests

$(TEST_BUILD):
	mkdir -p $@

TEST_SAN_STAMP := $(TEST_BUILD)/.san-$(if $(filter 0,$(TEST_SAN)),off,on)

$(TEST_SAN_STAMP): | $(TEST_BUILD)
	@rm -f $(TEST_BUILD)/.san-* && touch $@

TEST_HDRS := $(shell find kernel system_api tests -name '*.h' 2>/dev/null)

TEST_USER_DEPS := $(shell find user_space/library user_space/libc -name '*.c' \
                          -o -name '*.h' 2>/dev/null)

$(TEST_BIN): $(TEST_SRCS) $(TEST_HDRS) $(TEST_USER_DEPS) $(TEST_SAN_STAMP) | $(TEST_BUILD)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(TEST_SRCS)

BLOCK_CACHE_SRCS := tests/blockcache/block_cache_test.c tests/runner.c \
                    tests/fakes/fake_block_backends.c tests/fakes/fake_panic.c \
                    tests/fakes/fake_klog.c tests/fakes/fake_spinlock.c \
                    tests/fakes/fake_pmm.c tests/fakes/fake_pit.c \
                    kernel/drivers/block_device.c kernel/library/kernel_library.c

BLOCK_CACHE_BIN := $(TEST_BUILD)/leanos-block-cache

$(BLOCK_CACHE_BIN): $(BLOCK_CACHE_SRCS) $(TEST_HDRS) $(TEST_SAN_STAMP) | $(TEST_BUILD)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(BLOCK_CACHE_SRCS)

# M194: leanfs over the real block layer and its journal, with the power cut
# at forty-eight points across a workload.
JOURNAL_CRASH_SRCS := tests/journalcrash/journal_crash_test.c tests/runner.c \
                      tests/fakes/fake_block_backends.c \
                      $(filter-out tests/fakes/fake_blk.c,$(TEST_FAKES)) \
                      kernel/drivers/block_device.c kernel/file_system/leanfs.c \
                      kernel/library/kernel_library.c kernel/memory_management/heap.c

JOURNAL_CRASH_BIN := $(TEST_BUILD)/leanos-journal-crash

$(JOURNAL_CRASH_BIN): $(JOURNAL_CRASH_SRCS) $(TEST_HDRS) $(TEST_SAN_STAMP) | $(TEST_BUILD)
	$(HOSTCC) $(TEST_CFLAGS) -o $@ $(JOURNAL_CRASH_SRCS)

test-fast: $(TEST_BIN) $(BLOCK_CACHE_BIN) $(JOURNAL_CRASH_BIN)
	@$(TEST_BIN) $(TEST_FILTER)
	@$(BLOCK_CACHE_BIN)
	@$(JOURNAL_CRASH_BIN) $(TEST_FILTER)

COV_BUILD  := $(BUILD)/coverage
LLVM_CC    := $(shell for c in /opt/homebrew/opt/llvm/bin/clang \
                               /usr/local/opt/llvm/bin/clang \
                               clang; do \
                        command -v $$c >/dev/null 2>&1 && echo $$c && break; \
                      done)
COV_CC     := $(LLVM_CC)
# The same -idirafter as TEST_CFLAGS above, and for the same reason: this
# coverage build compiles the same sources, so it needs the same way of
# reaching <libintl.h>. It did not have it, so `make coverage` had not run
# since M157 put libintl.c in TEST_USER_SRCS - a broken instrument nobody
# was watching, which is the kind this project is supposed to notice.
COV_CFLAGS := -std=c11 -g -O0 -Wall -Wextra -DLEANOS_HOST_TEST \
              -fprofile-instr-generate -fcoverage-mapping \
              -Itests -Itests/fakes -Ikernel -Isystem_api/include -Iuser_space/library \
              $(LVGL_INCLUDES) -idirafter user_space/libc/include

$(COV_BUILD):
	mkdir -p $@

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

FILE    ?= $(TEST_KERNEL_SRCS)
MUTANTS ?= 0
mutate:
	@python3 tools/mutate.py $(FILE) --limit $(MUTANTS)

FUZZ_BUILD  := $(BUILD)/fuzz
FUZZ_CC     := $(LLVM_CC)
FUZZ_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -DLEANOS_HOST_TEST \
               -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
               -Itests -Itests/fakes -Ikernel -Isystem_api/include
FUZZ_FAKES  := tests/fakes/fake_panic_abort.c tests/fakes/fake_klog.c \
               tests/fakes/fake_spinlock.c tests/fakes/fake_pmm.c \
               tests/fakes/fake_vmm.c tests/fakes/fake_blk.c \
               tests/fakes/fake_rtc.c tests/fakes/fake_net.c \
               tests/fakes/fake_pit.c tests/fakes/fake_socket.c \
              tests/fakes/fake_fwcfg.c tests/fakes/fake_pci.c \
              tests/fakes/fake_arch.c tests/fakes/fake_kernel_objects.c \
              tests/fakes/fake_wireless_glue.c tests/fakes/fake_framebuffer.c
# The fuzzers drive the network parsers and leanfs. The drivers below reach
# hardware through shims that only their own unit tests define, and the
# console draws with a font the fuzzers never link; none of them is a parser.
FUZZ_KERNEL_SRCS := $(filter-out kernel/drivers/designware_i2c.c kernel/drivers/intel_wireless.c \
                                 kernel/drivers/intel_wireless_transport.c kernel/drivers/console.c, \
                                 $(TEST_KERNEL_SRCS))
FUZZ_TARGETS := $(FUZZ_BUILD)/fuzz_net $(FUZZ_BUILD)/fuzz_leanfs

$(FUZZ_BUILD):
	mkdir -p $@

$(FUZZ_BUILD)/fuzz_net: tests/fuzz/fuzz_net.c $(FUZZ_FAKES) $(FUZZ_KERNEL_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_FAKES) $(FUZZ_KERNEL_SRCS)

$(FUZZ_BUILD)/fuzz_leanfs: tests/fuzz/fuzz_leanfs.c $(FUZZ_FAKES) $(FUZZ_KERNEL_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_FAKES) $(FUZZ_KERNEL_SRCS)

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

test:
	@./tools/run-tests.sh

test-full:
	@./tools/run-tests.sh --full

.PHONY: test-fast test test-full test-visual coverage coverage-check fuzz fuzz-run mutate

print-%:
	@echo $($*)

-include $(shell find $(KOBJ) $(UOBJ) $(TEST_BUILD) -name '*.d' 2>/dev/null)
-include $(BUILD)/leanfs-put.d

clean:
	rm -rf $(filter-out $(BUILD)/ovmf,$(wildcard $(BUILD)/*))

distclean:
	rm -rf $(BUILD)

.PHONY: print-esp-start-lba print-esp-sector-count
print-esp-start-lba:
	@echo $(ESP_START_LBA)

print-esp-sector-count:
	@echo $(ESP_SECTOR_COUNT)
