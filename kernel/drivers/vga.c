#include "vga.h"

#include <stddef.h>

#define VGA_MEM    ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_ATTR   0x0F  /* white on black */

static size_t vga_row = 0;
static size_t vga_col = 0;

static void vga_put_entry_at(char c, size_t x, size_t y) {
    VGA_MEM[y * VGA_WIDTH + x] = ((uint16_t)VGA_ATTR << 8) | (uint8_t)c;
}

static void vga_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            VGA_MEM[(y - 1) * VGA_WIDTH + x] = VGA_MEM[y * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        vga_put_entry_at(' ', x, VGA_HEIGHT - 1);
    }
    vga_row = VGA_HEIGHT - 1;
}

void vga_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            vga_put_entry_at(' ', x, y);
        }
    }
    vga_row = 0;
    vga_col = 0;
}

void vga_putc(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else {
        vga_put_entry_at(c, vga_col, vga_row);
        vga_col++;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
        }
    }
    if (vga_row >= VGA_HEIGHT) {
        vga_scroll();
    }
}

