#include "console.h"

#include <stdint.h>

#include "framebuffer.h"
#include "font8x16.h"

#define CONSOLE_FG 0x00E0E0E0u
#define CONSOLE_BG 0x00101018u
#define TAB_STOP   8u

static uint32_t cols;
static uint32_t rows;
static uint32_t current_col;
static uint32_t current_row;

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    uint8_t code = (uint8_t)c;
    if (code > 0x7F) {
        code = '?';
    }
    const uint8_t *glyph = font8x16[code];
    uint32_t base_x = col * FONT_WIDTH;
    uint32_t base_y = row * FONT_HEIGHT;

    for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT_WIDTH; x++) {
            uint32_t rgb = (bits & (0x80 >> x)) ? CONSOLE_FG : CONSOLE_BG;
            framebuffer_put_pixel(base_x + x, base_y + y, rgb);
        }
    }
}

static void newline(void) {
    current_col = 0;
    current_row++;
    if (current_row >= rows) {
        framebuffer_scroll_up(FONT_HEIGHT, CONSOLE_BG);
        current_row = rows - 1;
    }
}

void console_init(void) {
    cols = framebuffer_width() / FONT_WIDTH;
    rows = framebuffer_height() / FONT_HEIGHT;
    current_col = 0;
    current_row = 0;
    framebuffer_clear(CONSOLE_BG);
}

void console_putc(char c) {
    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        current_col = 0;
        return;
    }
    if (c == '\b') {
        if (current_col > 0) {
            current_col--;
            draw_glyph(current_col, current_row, ' ');
        }
        return;
    }
    if (c == '\t') {
        uint32_t next = ((current_col / TAB_STOP) + 1) * TAB_STOP;
        while (current_col < next && current_col < cols) {
            draw_glyph(current_col, current_row, ' ');
            current_col++;
        }
        if (current_col >= cols) {
            newline();
        }
        return;
    }

    draw_glyph(current_col, current_row, c);
    current_col++;
    if (current_col >= cols) {
        newline();
    }
}

void console_puts(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}
