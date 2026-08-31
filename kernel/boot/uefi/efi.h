/* kernel/boot/uefi/efi.h
 *
 * Hand-written UEFI base types, matching the layouts the UEFI Specification
 * mandates (verified field-for-field against the public edk2 reference
 * headers during development - the struct layouts below are dictated by
 * the spec's own ABI, not anyone's copyrightable expression of it, the
 * same sense in which kernel/mm/e820.h's e820_entry_t matches the
 * conventional E820 ABI). No GNU-EFI, no edk2 headers, nothing linked into
 * the OS image - the boot loader (boot.c) is built as a freestanding PE32+
 * EFI application against just these headers (see efi_proto.h and boot.c
 * for what actually uses these).
 *
 * Only the pieces boot.c needs are defined here - this is not a general
 * UEFI headers library.
 */
#pragma once

#include <stdint.h>

/* Every EFI-spec function is called with Microsoft's x64 calling
 * convention (RCX/RDX/R8/R9, caller-allocated shadow space) regardless of
 * host OS - firmware is always compiled that way. clang's ms_abi
 * attribute forces that convention for both the functions the firmware
 * hands us (call sites already match, since clang generates ms_abi calls
 * for a function pointer typed this way) and efi_main itself, which the
 * firmware calls into. */
#define EFIAPI __attribute__((ms_abi))

typedef uint64_t UINTN;
typedef int64_t INTN;
typedef uint8_t BOOLEAN;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int32_t INT32;
typedef uint16_t CHAR16;
typedef void VOID;
typedef UINTN EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;
typedef UINT64 EFI_LBA;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;

#define TRUE 1
#define FALSE 0
#define NULL ((void *)0)

/* High bit set marks failure for every EFI_STATUS - the specific codes
 * below are the only ones boot.c checks for by name. */
#define EFI_SUCCESS 0
#define EFI_BUFFER_TOO_SMALL ((EFI_STATUS)0x8000000000000005ULL)
#define EFI_ERROR(status) (((INTN)(status)) < 0)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

/* Precedes every standard EFI table (System/Boot/Runtime Services) -
 * boot.c never reads these fields itself, but EFI_SYSTEM_TABLE and
 * EFI_BOOT_SERVICES below embed this first, and every field after it only
 * lands at the right offset if this is exactly 24 bytes. */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* AllocatePages' Type argument. */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* Only the values boot.c actually branches on are named; the rest of the
 * UEFI-defined range (up to EfiMaxMemoryType) is sequential from 0 in
 * this exact order per the spec, so leaving them unnamed doesn't shift
 * any of these. */
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    /* M90: the four types past EfiConventionalMemory that build_e820_map
     * now has to tell apart. EfiUnusableMemory and EfiPalCode are RAM the
     * OS must not touch; the two ACPI types are RAM holding tables
     * kernel/acpi/acpi.c reads where they lie; the two MMIO types are not
     * memory at all, and the identity map must not cover them - see
     * kernel/mm/e820.h for the whole argument. */
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory
} EFI_MEMORY_TYPE;

/* GetMemoryMap's own descriptor format. Per spec, GetMemoryMap's
 * DescriptorSize *out* parameter is the authoritative stride through the
 * returned array - it may be larger than sizeof(EFI_MEMORY_DESCRIPTOR)
 * (room for future fields), so boot.c always walks the map with that
 * value, never this struct's own size. */
typedef struct {
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* LocateHandleBuffer's SearchType - boot.c only ever searches ByProtocol. */
typedef enum {
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;
