#include "panic.h"

#include "drivers/vga.h"

void panic(const char *msg) {
    vga_puts("\n*** KERNEL PANIC: ");
    vga_puts(msg);
    vga_puts(" ***\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
