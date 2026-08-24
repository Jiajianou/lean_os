/* kernel/boot/uefi/boot.c
 *
 * The only boot loader (M26 removed the earlier from-scratch BIOS path -
 * see milestones.md). Firmware loads and runs this as a PE32+ EFI
 * application, from the ESP the top-level Makefile formats into the disk
 * image (kernel/boot/mbr.asm's partition table is what points UEFI at it).
 * Its whole job: collect a memory map, set up a linear framebuffer, load
 * the kernel binary, and jump into it with exactly the handoff
 * kernel_main() expects - RDI a pointer to the {count, entries[]} e820
 * layout kernel/mm/e820.h documents, RSI a kernel/drivers/fb.h
 * fb_boot_info_t. Producing that handoff is what lets kernel.c/pmm.c/fb.c
 * stay completely unaware of how they got there.
 *
 * KERNEL_SECTOR_COUNT is supplied by the Makefile via -D, read from
 * build/kernel.sectors (a side effect of the kernel.bin build step) since
 * it's only known once the kernel is actually compiled and objcopy'd.
 */
#include "efi.h"
#include "efi_proto.h"

#ifndef KERNEL_SECTOR_COUNT
#define KERNEL_SECTOR_COUNT 32 /* placeholder so editors/IDE tooling can parse this file standalone */
#endif

/* LBA 0 is the disk's MBR (kernel/boot/mbr.asm) - pure partition-table
 * data, not executed by anything - so the kernel blob starts right after
 * it, at LBA 1. Must match the top-level Makefile's image-assembly recipe
 * exactly (mbr.bin, one sector, then kernel.bin). */
#define KERNEL_START_LBA 1
#define KERNEL_LOAD_ADDR 0x100000ULL /* matches kernel/linker.ld's load address */

#define PAGE_SIZE 4096ULL

/* ---- e820-format handoff buffer (kernel/mm/e820.h) ----
 * count (dword) + 4 bytes padding + that many e820_entry_t records,
 * exactly the layout build_e820_and_exit_boot_services below fills in.
 * Sized generously above whatever GetMemoryMap first reports needing -
 * see build_e820_map(). */
typedef struct __attribute__((packed)) {
    UINT64 base;
    UINT64 length;
    UINT32 type;
    UINT32 acpi_ext;
} e820_entry_t;

#define E820_TYPE_USABLE 1
#define E820_TYPE_RESERVED 2

typedef struct __attribute__((packed)) {
    UINT32 count;
    UINT32 reserved;
    e820_entry_t entries[]; /* flexible array - matches the on-disk/handoff layout exactly */
} e820_map_t;

/* Matches kernel/drivers/fb.h's fb_boot_info_t field-for-field. */
typedef struct __attribute__((packed)) {
    UINT64 phys_addr;
    UINT32 pitch;
    UINT32 width;
    UINT32 height;
    UINT32 bpp;
} fb_boot_info_t;

static EFI_SYSTEM_TABLE *gST;

static void puts16(CHAR16 *s) {
    gST->ConOut->OutputString(gST->ConOut, s);
}

static void halt(CHAR16 *msg) {
    puts16(msg);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* ---- Graphics Output: find/select a 0x00RRGGBB-compatible mode and
 * report its framebuffer - see efi_proto.h's pixel-format comment for
 * why PixelBlueGreenRedReserved8BitPerColor is the one fb.c needs. ---- */
static void init_framebuffer(fb_boot_info_t *fb) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (EFI_ERROR(gST->BootServices->LocateProtocol(&gop_guid, NULL, (void **)&gop)) || !gop) {
        halt(u"lean_os uefi: no Graphics Output Protocol available\r\n");
    }

    /* Prefer an exact 1024x768 match in the byte order fb.c expects;
     * otherwise take the first mode in that byte order at all, whatever
     * its resolution - a working framebuffer beats a preferred one that
     * doesn't exist on this firmware. */
    INT32 best_exact = -1;
    INT32 best_any = -1;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN info_size = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (EFI_ERROR(gop->QueryMode(gop, m, &info_size, &info)) || !info) {
            continue;
        }
        if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
            if (best_any < 0) {
                best_any = (INT32)m;
            }
            if (info->HorizontalResolution == 1024 && info->VerticalResolution == 768) {
                best_exact = (INT32)m;
            }
        }
    }

    INT32 chosen = (best_exact >= 0) ? best_exact : best_any;
    if (chosen < 0) {
        halt(u"lean_os uefi: no 0x00RRGGBB (BGR8888) graphics mode found\r\n");
    }
    if ((UINT32)chosen != gop->Mode->Mode) {
        if (EFI_ERROR(gop->SetMode(gop, (UINT32)chosen))) {
            halt(u"lean_os uefi: SetMode failed\r\n");
        }
    }

    fb->phys_addr = gop->Mode->FrameBufferBase;
    fb->pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    fb->width = gop->Mode->Info->HorizontalResolution;
    fb->height = gop->Mode->Info->VerticalResolution;
    fb->bpp = 32;
}

/* ---- Block I/O: locate the whole physical disk this app was itself
 * loaded from (not the ESP partition child handle LoadedImage->
 * DeviceHandle actually is) and read the kernel blob's fixed LBA range
 * from it. Identified by UEFI device path, not "the first non-partition
 * BlockIo handle" (which happens to work under QEMU only because a QEMU
 * VM only ever has the one disk this project attaches - a real machine
 * with an internal drive *and* the USB stick this is meant to boot from
 * would make that guess a coin flip). A device path is a length-prefixed
 * node list terminated by an End-Entire-Device-Path node; the ESP
 * partition's path is the whole disk's own path plus one trailing
 * partition node, so chopping that last node off the path we were loaded
 * from gives an exact byte-for-byte prefix every *other* handle on the
 * same physical disk shares - unique to the disk itself only when a
 * candidate's path is *exactly* that prefix and nothing more (the whole
 * disk handle has no partition node of its own to chop). ---- */
static UINT16 device_path_node_length(const EFI_DEVICE_PATH_PROTOCOL *node) {
    return (UINT16)(node->Length[0] | ((UINT16)node->Length[1] << 8));
}

static int device_path_is_end(const EFI_DEVICE_PATH_PROTOCOL *node) {
    return node->Type == EFI_DEVICE_PATH_TYPE_END && node->SubType == EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE;
}

/* Byte offset of the last real node before the End node - i.e. the length
 * of "everything except the final node", which for an ESP's device path
 * is exactly the disk's own device path length. */
static UINTN device_path_size_without_last_node(const EFI_DEVICE_PATH_PROTOCOL *path) {
    UINTN offset = 0;
    UINTN last_node_offset = 0;
    const EFI_DEVICE_PATH_PROTOCOL *node = path;
    while (!device_path_is_end(node)) {
        last_node_offset = offset;
        UINT16 len = device_path_node_length(node);
        offset += len;
        node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)node + len);
    }
    return last_node_offset;
}

static int bytes_equal(const void *a, const void *b, UINTN n) {
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    for (UINTN i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return 0;
        }
    }
    return 1;
}

static EFI_BLOCK_IO_PROTOCOL *find_whole_disk_block_io(EFI_HANDLE image_handle) {
    EFI_BOOT_SERVICES *bs = gST->BootServices;
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID blockio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    if (EFI_ERROR(bs->HandleProtocol(image_handle, &loaded_image_guid, (void **)&loaded_image)) || !loaded_image) {
        return NULL;
    }
    EFI_DEVICE_PATH_PROTOCOL *our_path = NULL;
    if (EFI_ERROR(bs->HandleProtocol(loaded_image->DeviceHandle, &device_path_guid, (void **)&our_path)) || !our_path) {
        return NULL;
    }
    UINTN disk_path_len = device_path_size_without_last_node(our_path);

    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(bs->LocateHandleBuffer(ByProtocol, &blockio_guid, NULL, &count, &handles))) {
        return NULL;
    }
    for (UINTN i = 0; i < count; i++) {
        EFI_DEVICE_PATH_PROTOCOL *candidate_path = NULL;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], &device_path_guid, (void **)&candidate_path)) || !candidate_path) {
            continue;
        }
        /* A match is "candidate's device path is exactly our disk prefix,
         * then immediately terminated" - i.e. an End node sits right at
         * offset disk_path_len, and everything before that is identical
         * to our own path's prefix. Not "candidate's total length equals
         * disk_path_len": disk_path_len itself deliberately excludes any
         * End node (it's a byte count of real nodes only), so comparing
         * it against a *terminated* path's total length would never
         * match anything, correct disk included. */
        const EFI_DEVICE_PATH_PROTOCOL *candidate_end =
            (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)candidate_path + disk_path_len);
        if (!device_path_is_end(candidate_end) || !bytes_equal(candidate_path, our_path, disk_path_len)) {
            continue;
        }
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], &blockio_guid, (void **)&bio)) || !bio) {
            continue;
        }
        if (!bio->Media->LogicalPartition && bio->Media->MediaPresent) {
            return bio;
        }
    }
    return NULL;
}

static void load_kernel(EFI_HANDLE image_handle) {
    EFI_BLOCK_IO_PROTOCOL *bio = find_whole_disk_block_io(image_handle);
    if (!bio) {
        halt(u"lean_os uefi: no whole-disk Block I/O protocol found\r\n");
    }

    UINTN kernel_bytes = (UINTN)KERNEL_SECTOR_COUNT * 512;
    UINTN pages = (kernel_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS addr = KERNEL_LOAD_ADDR;
    if (EFI_ERROR(gST->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr))) {
        halt(u"lean_os uefi: could not reserve kernel load address (0x100000)\r\n");
    }

    /* bio->Media->BlockSize is assumed 512 here, same as every LBA math
     * elsewhere in this project (leanfs, mbr.asm's partition entry) - true
     * for every disk QEMU's IDE/AHCI emulation presents. */
    if (EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, KERNEL_START_LBA, kernel_bytes,
                                   (void *)(UINTN)KERNEL_LOAD_ADDR))) {
        halt(u"lean_os uefi: ReadBlocks failed loading the kernel\r\n");
    }
}

/* ---- Memory map: GetMemoryMap + ExitBootServices, converted into the
 * exact e820_map_t layout kernel_main/pmm_init already parse. Buffers are
 * allocated with slack because AllocatePool for those buffers is itself
 * an allocation that can add a descriptor to the very map being fetched -
 * standard UEFI bootloader dance, retried until ExitBootServices actually
 * accepts the MapKey it was just handed. ---- */
#define MAP_SLACK_DESCRIPTORS 8

static e820_map_t *build_e820_and_exit_boot_services(EFI_HANDLE image_handle) {
    EFI_BOOT_SERVICES *bs = gST->BootServices;

    UINTN map_capacity = 0; /* bytes currently allocated for `map` */
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN e820_capacity = 0; /* entries currently allocated for `e820` */
    e820_map_t *e820 = NULL;

    /* GetMemoryMap/AllocatePool/FreePool all change the very memory map
     * being described, which invalidates any MapKey obtained before them
     * - so the rule this loop follows is: the moment any of those three
     * run, `continue` back to the top and re-fetch, and never let
     * anything (not even a console print - ConOut's own driver is free
     * to allocate) run between the GetMemoryMap call that produced the
     * MapKey actually passed to ExitBootServices and that call itself. */
    for (;;) {
        UINTN map_size = map_capacity;
        UINTN map_key, desc_size;
        UINT32 desc_version;
        EFI_STATUS status = bs->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_version);

        if (status == EFI_BUFFER_TOO_SMALL) {
            if (map) {
                bs->FreePool(map);
            }
            map_capacity = map_size + MAP_SLACK_DESCRIPTORS * desc_size;
            if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, map_capacity, (void **)&map))) {
                halt(u"lean_os uefi: out of pool memory for the UEFI memory map\r\n");
            }
            continue;
        }
        if (EFI_ERROR(status)) {
            halt(u"lean_os uefi: GetMemoryMap failed\r\n");
        }

        UINTN entries_now = map_size / desc_size;
        if (entries_now > e820_capacity) {
            if (e820) {
                bs->FreePool(e820);
            }
            e820_capacity = entries_now + MAP_SLACK_DESCRIPTORS;
            UINTN e820_bytes = 8 + e820_capacity * sizeof(e820_entry_t);
            if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, e820_bytes, (void **)&e820))) {
                halt(u"lean_os uefi: out of pool memory for the e820 handoff buffer\r\n");
            }
            continue; /* that AllocatePool just invalidated map_key above */
        }

        /* Pure computation from here on - no firmware calls until
         * ExitBootServices itself, so map_key stays valid. */
        UINT32 n = 0;
        for (UINTN off = 0; off < map_size && n < e820_capacity; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + off);
            e820->entries[n].base = d->PhysicalStart;
            e820->entries[n].length = d->NumberOfPages * PAGE_SIZE;
            e820->entries[n].acpi_ext = 1;
            switch (d->Type) {
                case EfiLoaderCode:
                case EfiLoaderData:
                case EfiBootServicesCode:
                case EfiBootServicesData:
                case EfiConventionalMemory:
                    e820->entries[n].type = E820_TYPE_USABLE;
                    break;
                default:
                    e820->entries[n].type = E820_TYPE_RESERVED;
                    break;
            }
            n++;
        }
        e820->count = n;
        e820->reserved = 0;

        status = bs->ExitBootServices(image_handle, map_key);
        if (!EFI_ERROR(status)) {
            return e820;
        }
        /* EFI_INVALID_PARAMETER: an ExitBootServices notification callback
         * perturbed the map as a side effect of this very call (common,
         * spec-anticipated, and why every real UEFI OS loader retries
         * this exact way) - loop and re-fetch. */
    }
}

/* M47: the ACPI 2.0 RSDP GUID (8868E871-E4F1-11D3-BC22-0080C73C8881) and
 * the ACPI 1.0 one (EB9D2D30-2D88-11D3-9A16-0090273FC14D). Under UEFI the
 * RSDP is handed over in the system table's configuration array - it is
 * NOT in the legacy EBDA/0xE0000 range kernel/acpi/acpi.c scans, and
 * firmware is under no obligation to leave a copy there.
 *
 * That was a real, silent regression: M26 made UEFI the only boot path,
 * and from that moment acpi_find_madt found nothing on every boot, so SMP
 * has been quietly falling back to single-core ever since. It surfaced
 * here because M47 needs the FADT for S5 and the log said "no FADT
 * found". Passing the pointer through costs one register in the handoff. */
static const EFI_GUID ACPI2_RSDP_GUID = {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static const EFI_GUID ACPI1_RSDP_GUID = {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (a->Data4[i] != b->Data4[i]) {
            return 0;
        }
    }
    return 1;
}

/* The ACPI 2.0 table wins if both are present (it is the one with the
 * XSDT), which is why this scans for it first rather than taking whatever
 * turns up. 0 if the firmware published neither, which the kernel already
 * treats as "no ACPI on this platform". */
static UINTN find_rsdp(EFI_SYSTEM_TABLE *st) {
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &ACPI2_RSDP_GUID)) {
            return (UINTN)st->ConfigurationTable[i].VendorTable;
        }
    }
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &ACPI1_RSDP_GUID)) {
            return (UINTN)st->ConfigurationTable[i].VendorTable;
        }
    }
    return 0;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gST = SystemTable;
    puts16(u"lean_os uefi: booting...\r\n");

    /* Read before ExitBootServices: the configuration table is firmware
     * memory, and nothing about it is guaranteed reachable afterwards. */
    UINTN rsdp = find_rsdp(SystemTable);

    static fb_boot_info_t fb;
    init_framebuffer(&fb);

    puts16(u"lean_os uefi: framebuffer ready, loading kernel...\r\n");
    load_kernel(ImageHandle);

    puts16(u"lean_os uefi: exiting boot services, jumping to kernel...\r\n");
    e820_map_t *e820 = build_e820_and_exit_boot_services(ImageHandle);

    /* No firmware calls are safe past this point - ConOut/BootServices no
     * longer exist. Jump straight into the kernel with the exact System V
     * AMD64 register handoff kernel/arch/x86_64/entry.asm's _start
     * expects (RDI = e820 map, RSI = fb_boot_info_t) - matching
     * kernel_main's signature is done here in raw asm rather than a C
     * call because this whole file, including efi_main itself, is
     * compiled ms_abi (the calling convention every UEFI firmware call
     * requires); the kernel image was built as an ordinary System V
     * ELF64 (kernel/linker.ld) and knows nothing about ms_abi.
     *
     * M47: RDX carries the RSDP physical address (0 if the firmware
     * published none) - the third System V integer argument, so
     * kernel_main just grows a third parameter. */
    __asm__ volatile(
        "mov %0, %%rdi\n\t"
        "mov %1, %%rsi\n\t"
        "mov %3, %%rdx\n\t"
        "jmp *%2\n\t"
        :
        : "r"((UINTN)e820), "r"((UINTN)&fb), "r"((UINTN)KERNEL_LOAD_ADDR), "r"(rsdp)
        : "rdi", "rsi", "rdx");

    __builtin_unreachable();
}
