/* kernel/mm/e820.h
 *
 * e820-format memory map the boot loader (kernel/boot/uefi/boot.c,
 * build_e820_and_exit_boot_services) hands off in RDI: a dword entry count
 * immediately followed by that many e820_entry_t records. Shared between
 * kernel.c (prints it), pmm.c (seeds the physical frame allocator from
 * it) and vmm.c (M90: builds the identity map from it) so the layout is
 * defined exactly once.
 *
 * M90: the type field used to carry two values - usable, and everything
 * else - because the only question anyone asked of it was "may the frame
 * allocator hand this out". Extending the identity map past 1 GiB asks a
 * second question that the collapsed type cannot answer: *is this address
 * memory at all*. The kernel dereferences physical addresses it never
 * allocated - ACPI tables (kernel/acpi/acpi.c) sit in firmware-reserved
 * RAM and are read where they lie - so the identity map has to cover more
 * than the usable ranges. It must equally not cover MMIO: the framebuffer
 * (drivers/fb.c) and the local APIC (arch/x86_64/lapic.c) map their own
 * device pages 4 KiB at a time, and vmm_map_page panics outright on an
 * address already covered by a 2 MiB huge page. "Reserved RAM" and
 * "device memory" were the same number and had to stop being.
 *
 * Types 1-4 are the ACPI-specified e820 values and mean what they mean
 * everywhere. Type 6 is this project's own: the e820 standard has no way
 * to say "memory-mapped I/O" because a BIOS simply omitted those ranges,
 * and UEFI's memory map does describe them - so rather than throw that
 * information away to match a legacy encoding, it is kept under a number
 * the standard leaves unused.
 */
#pragma once

#include <stdint.h>

#define E820_TYPE_USABLE       1
#define E820_TYPE_RESERVED     2 /* real memory, owned by firmware - not the allocator's to hand out */
#define E820_TYPE_ACPI_RECLAIM 3 /* real memory holding ACPI tables, which acpi.c reads in place */
#define E820_TYPE_ACPI_NVS     4 /* real memory the firmware needs preserved */
#define E820_TYPE_MMIO         6 /* NOT memory: a device's registers or framebuffer aperture (M90, this project's own type) */

/* Is this range backed by RAM - i.e. may the identity map cover it?
 * Everything except MMIO is, including the ranges nothing may allocate:
 * the distinction this answers is "can the CPU read it as memory", which
 * is a different question from "is it free". */
static inline int e820_is_ram(uint32_t type) {
    return type != E820_TYPE_MMIO;
}

/* Matches the 24-byte layout boot.c's build_e820_and_exit_boot_services
 * fills in: base(8) + length(8) + type(4) + ACPI 3.x extended attribute(4). */
typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_ext;
} e820_entry_t;

/* M90: the handoff buffer's own header, so a caller can walk the entries
 * without repeating the "+ 8" that the count-plus-padding layout implies.
 * Written as a helper rather than a struct because the buffer arrives as
 * a `uint32_t *` from assembly and every existing caller already spells
 * it that way. */
static inline uint32_t e820_count(const uint32_t *map) {
    return *map;
}

static inline const e820_entry_t *e820_entries(const uint32_t *map) {
    return (const e820_entry_t *)((const uint8_t *)map + 8);
}
