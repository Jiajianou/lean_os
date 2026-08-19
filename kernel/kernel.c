#include <stdint.h>

#include "drivers/vga.h"
#include "panic.h"

/* Matches the 24-byte layout stage2.asm's collect_e820_map fills in:
 * base(8) + length(8) + type(4) + ACPI 3.x extended attribute(4). */
typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_ext;
} e820_entry_t;

/* e820_map: pointer to a dword entry count immediately followed by that
 * many e820_entry_t records — the layout stage2.asm builds at
 * E820_COUNT_ADDR and hands off in RDI. */
void kernel_main(uint32_t *e820_map) {
    vga_clear();
    vga_puts("lean_os kernel: hello from C!\n\n");

    uint32_t count = *e820_map;
    if (count == 0) {
        panic("E820 memory map is empty - cannot continue");
    }

    e820_entry_t *entries = (e820_entry_t *)((uint8_t *)e820_map + 8);

    vga_puts("E820 memory map (");
    vga_put_hex32(count);
    vga_puts(" entries):\n");

    for (uint32_t i = 0; i < count; i++) {
        vga_puts("  base=0x");
        vga_put_hex64(entries[i].base);
        vga_puts(" len=0x");
        vga_put_hex64(entries[i].length);
        vga_puts(" type=0x");
        vga_put_hex32(entries[i].type);
        vga_putc('\n');
    }

    vga_puts("\nNo scheduler yet - halting.\n");
}
