#include "efi.h"
#include "efi_proto.h"

#include "boot/boot_options.h"

#ifndef KERNEL_SECTOR_COUNT
#define KERNEL_SECTOR_COUNT 32
#endif

#define KERNEL_START_LBA 1
#define KERNEL_LOAD_ADDRESS 0x100000ULL

#define PAGE_SIZE 4096ULL

typedef struct __attribute__((packed)) {
    UINT64 base;
    UINT64 length;
    UINT32 type;
    UINT32 acpi_ext;
} e820_entry_t;

#define E820_TYPE_USABLE 1
#define E820_TYPE_RESERVED 2
#define E820_TYPE_ACPI_RECLAIM 3
#define E820_TYPE_ACPI_NVS 4
#define E820_TYPE_MMIO 6

typedef struct __attribute__((packed)) {
    UINT32 count;
    UINT32 reserved;
    e820_entry_t entries[];
} e820_map_t;

typedef struct __attribute__((packed)) {
    UINT64 phys_address;
    UINT32 pitch;
    UINT32 width;
    UINT32 height;
    UINT32 bpp;
} framebuffer_boot_info_t;

static EFI_SYSTEM_TABLE *gST;

static void puts16(CHAR16 *s) {
    gST->ConOut->OutputString(gST->ConOut, s);
}

static void halt(CHAR16 *message) {
    puts16(message);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static boot_options_t boot_options;

static void read_boot_options(EFI_HANDLE image_handle) {
    boot_options_defaults(&boot_options);

    EFI_BOOT_SERVICES *bs = gST->BootServices;
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID file_system_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    if (EFI_ERROR(bs->HandleProtocol(image_handle, &loaded_image_guid, (void **)&loaded_image)) || !loaded_image) {
        return;
    }
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *file_system = NULL;
    if (EFI_ERROR(bs->HandleProtocol(loaded_image->DeviceHandle, &file_system_guid, (void **)&file_system)) ||
        !file_system) {
        return;
    }
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(file_system->OpenVolume(file_system, &root)) || !root) {
        return;
    }
    EFI_FILE_PROTOCOL *file = NULL;
    if (EFI_ERROR(root->Open(root, &file, u"\\EFI\\BOOT\\lean_os.cfg", EFI_FILE_MODE_READ, 0)) || !file) {
        root->Close(root);
        return;
    }

    static char text[4096];
    UINTN size = sizeof(text);
    EFI_STATUS status = file->Read(file, &size, text);
    file->Close(file);
    root->Close(root);
    if (EFI_ERROR(status)) {
        return;
    }
    if (size > sizeof(text)) {
        size = sizeof(text);
    }
    boot_options_parse(text, (UINT32)size, &boot_options);
    puts16(u"lean_os uefi: \\EFI\\BOOT\\lean_os.cfg read\r\n");
}

static INT32 choose_graphics_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 *out_width,
                                  UINT32 *out_height) {
    INT32 chosen = -1;
    int chosen_score = 0;
    boot_options.offered_count = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        UINTN info_size = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (EFI_ERROR(gop->QueryMode(gop, m, &info_size, &info)) || !info) {
            continue;
        }
        if (info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) {
            continue;
        }
        if (boot_options.offered_count < BOOT_OFFERED_MODES_MAX) {
            boot_options.offered[boot_options.offered_count][0] = info->HorizontalResolution;
            boot_options.offered[boot_options.offered_count][1] = info->VerticalResolution;
            boot_options.offered_count++;
        }
        int score = boot_options_video_score(&boot_options, info->HorizontalResolution,
                                             info->VerticalResolution);
        if (score < 0) {
            continue;
        }
        if (chosen < 0 || score > chosen_score) {
            chosen = (INT32)m;
            chosen_score = score;
            *out_width = info->HorizontalResolution;
            *out_height = info->VerticalResolution;
        }
    }
    return chosen;
}

static void init_framebuffer(framebuffer_boot_info_t *framebuffer) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (EFI_ERROR(gST->BootServices->LocateProtocol(&gop_guid, NULL, (void **)&gop)) || !gop) {
        halt(u"lean_os uefi: no Graphics Output Protocol available\r\n");
    }

    UINT32 chosen_width = 0;
    UINT32 chosen_height = 0;
    INT32 chosen = -1;

    if (boot_options.video_selection == BOOT_VIDEO_FIRMWARE) {
        if (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor &&
            gop->Mode->Info->HorizontalResolution > 0) {
            chosen = (INT32)gop->Mode->Mode;
            chosen_width = gop->Mode->Info->HorizontalResolution;
            chosen_height = gop->Mode->Info->VerticalResolution;
        } else {
            puts16(u"lean_os uefi: the firmware's own mode is not one this OS can draw in - "
                   u"taking the largest\r\n");
            boot_options.video_selection = BOOT_VIDEO_LARGEST;
            boot_options.unknown_keys++;
        }
    }
    if (chosen < 0) {
        chosen = choose_graphics_mode(gop, &chosen_width, &chosen_height);
    }

    if (chosen < 0 && boot_options.video_selection == BOOT_VIDEO_EXACT) {
        puts16(u"lean_os uefi: the video= mode is not one this firmware has - taking the largest\r\n");
        boot_options.video_selection = BOOT_VIDEO_LARGEST;
        boot_options.unknown_keys++;
        chosen = choose_graphics_mode(gop, &chosen_width, &chosen_height);
    }

    if (chosen < 0) {
        halt(u"lean_os uefi: no 0x00RRGGBB (BGR8888) graphics mode found\r\n");
    }
    if ((UINT32)chosen != gop->Mode->Mode) {
        if (EFI_ERROR(gop->SetMode(gop, (UINT32)chosen))) {
            halt(u"lean_os uefi: SetMode failed\r\n");
        }
    }

    boot_options.chosen_width = chosen_width;
    boot_options.chosen_height = chosen_height;

    framebuffer->phys_address = gop->Mode->FrameBufferBase;
    framebuffer->pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    framebuffer->width = gop->Mode->Info->HorizontalResolution;
    framebuffer->height = gop->Mode->Info->VerticalResolution;
    framebuffer->bpp = 32;
}

static UINT16 device_path_node_length(const EFI_DEVICE_PATH_PROTOCOL *node) {
    return (UINT16)(node->Length[0] | ((UINT16)node->Length[1] << 8));
}

static int device_path_is_end(const EFI_DEVICE_PATH_PROTOCOL *node) {
    return node->Type == EFI_DEVICE_PATH_TYPE_END && node->SubType == EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE;
}

static UINTN device_path_size_without_last_node(const EFI_DEVICE_PATH_PROTOCOL *path) {
    UINTN offset = 0;
    UINTN last_node_offset = 0;
    const EFI_DEVICE_PATH_PROTOCOL *node = path;
    while (!device_path_is_end(node)) {
        last_node_offset = offset;
        UINT16 length = device_path_node_length(node);
        offset += length;
        node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)node + length);
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
    UINTN disk_path_length = device_path_size_without_last_node(our_path);

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
        const EFI_DEVICE_PATH_PROTOCOL *candidate_end =
            (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)candidate_path + disk_path_length);
        if (!device_path_is_end(candidate_end) || !bytes_equal(candidate_path, our_path, disk_path_length)) {
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
    UINTN pages = KERNEL_IMAGE_PAGES;
    if (pages < (kernel_bytes + PAGE_SIZE - 1) / PAGE_SIZE) {
        halt(u"lean_os uefi: KERNEL_IMAGE_PAGES is smaller than the kernel on disk\r\n");
    }
    EFI_PHYSICAL_ADDRESS address = KERNEL_LOAD_ADDRESS;
    if (EFI_ERROR(gST->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, pages, &address))) {
        halt(u"lean_os uefi: could not reserve kernel load address (0x100000)\r\n");
    }

    if (EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, KERNEL_START_LBA, kernel_bytes,
                                   (void *)(UINTN)KERNEL_LOAD_ADDRESS))) {
        halt(u"lean_os uefi: ReadBlocks failed loading the kernel\r\n");
    }
}

#define MAP_SLACK_DESCRIPTORS 8

static e820_map_t *build_e820_and_exit_boot_services(EFI_HANDLE image_handle) {
    EFI_BOOT_SERVICES *bs = gST->BootServices;

    UINTN map_capacity = 0;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN e820_capacity = 0;
    e820_map_t *e820 = NULL;

    for (;;) {
        UINTN map_size = map_capacity;
        UINTN map_key, descriptor_size;
        UINT32 descriptor_version;
        EFI_STATUS status = bs->GetMemoryMap(&map_size, map, &map_key, &descriptor_size, &descriptor_version);

        if (status == EFI_BUFFER_TOO_SMALL) {
            if (map) {
                bs->FreePool(map);
            }
            map_capacity = map_size + MAP_SLACK_DESCRIPTORS * descriptor_size;
            if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, map_capacity, (void **)&map))) {
                halt(u"lean_os uefi: out of pool memory for the UEFI memory map\r\n");
            }
            continue;
        }
        if (EFI_ERROR(status)) {
            halt(u"lean_os uefi: GetMemoryMap failed\r\n");
        }

        UINTN entries_now = map_size / descriptor_size;
        if (entries_now > e820_capacity) {
            if (e820) {
                bs->FreePool(e820);
            }
            e820_capacity = entries_now + MAP_SLACK_DESCRIPTORS;
            UINTN e820_bytes = 8 + e820_capacity * sizeof(e820_entry_t);
            if (EFI_ERROR(bs->AllocatePool(EfiLoaderData, e820_bytes, (void **)&e820))) {
                halt(u"lean_os uefi: out of pool memory for the e820 handoff buffer\r\n");
            }
            continue;
        }

        UINT32 n = 0;
        for (UINTN off = 0; off < map_size && n < e820_capacity; off += descriptor_size) {
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
                case EfiACPIReclaimMemory:
                    e820->entries[n].type = E820_TYPE_ACPI_RECLAIM;
                    break;
                case EfiACPIMemoryNVS:
                    e820->entries[n].type = E820_TYPE_ACPI_NVS;
                    break;
                case EfiMemoryMappedIO:
                case EfiMemoryMappedIOPortSpace:
                    e820->entries[n].type = E820_TYPE_MMIO;
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
    }
}

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

    UINTN rsdp = find_rsdp(SystemTable);

    read_boot_options(ImageHandle);

    static framebuffer_boot_info_t framebuffer;
    init_framebuffer(&framebuffer);

    puts16(u"lean_os uefi: framebuffer ready, loading kernel...\r\n");
    load_kernel(ImageHandle);

    puts16(u"lean_os uefi: exiting boot services, jumping to kernel...\r\n");
    e820_map_t *e820 = build_e820_and_exit_boot_services(ImageHandle);

    __asm__ volatile(
        "mov %0, %%rdi\n\t"
        "mov %1, %%rsi\n\t"
        "mov %3, %%rdx\n\t"
        "mov %4, %%r8\n\t"
        "jmp *%2\n\t"
        :
        : "r"((UINTN)e820), "r"((UINTN)&framebuffer), "r"((UINTN)KERNEL_LOAD_ADDRESS), "r"(rsdp),
          "r"((UINTN)&boot_options)
        : "rdi", "rsi", "rdx", "r8");

    __builtin_unreachable();
}
