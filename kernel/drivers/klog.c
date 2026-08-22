#include "klog.h"

#include "serial.h"
#include "vga.h"

void klog_init(void) {
    vga_clear();
    serial_init();
}

void klog_putc(char c) {
    vga_putc(c);
    serial_putc(c);
}

void klog_puts(const char *s) {
    vga_puts(s);
    serial_puts(s);
}

static char hex_digit(uint8_t nibble) {
    return (char)(nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10));
}

void klog_put_hex32(uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        klog_putc(hex_digit((value >> shift) & 0xF));
    }
}

void klog_put_hex64(uint64_t value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        klog_putc(hex_digit((value >> shift) & 0xF));
    }
}
