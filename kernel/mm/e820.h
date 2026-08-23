/* kernel/mm/e820.h
 *
 * e820-format memory map the boot loader (kernel/boot/uefi/boot.c,
 * build_e820_and_exit_boot_services) hands off in RDI: a dword entry count
 * immediately followed by that many e820_entry_t records. Shared between
 * kernel.c (prints it) and pmm.c (seeds the physical frame allocator from
 * it) so the layout is defined exactly once.
 */
#pragma once

#include <stdint.h>

#define E820_TYPE_USABLE 1

/* Matches the 24-byte layout boot.c's build_e820_and_exit_boot_services
 * fills in: base(8) + length(8) + type(4) + ACPI 3.x extended attribute(4). */
typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_ext;
} e820_entry_t;
