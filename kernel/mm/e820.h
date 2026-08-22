/* kernel/mm/e820.h
 *
 * BIOS memory map handed off from stage2.asm (collect_e820_map) in RDI: a
 * dword entry count immediately followed by that many e820_entry_t
 * records. Shared between kernel.c (prints it) and pmm.c (seeds the
 * physical frame allocator from it) so the layout is defined exactly once.
 */
#pragma once

#include <stdint.h>

#define E820_TYPE_USABLE 1

/* Matches the 24-byte layout stage2.asm's collect_e820_map fills in:
 * base(8) + length(8) + type(4) + ACPI 3.x extended attribute(4). */
typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_ext;
} e820_entry_t;
