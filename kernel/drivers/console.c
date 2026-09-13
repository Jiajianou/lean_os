#include "console.h"

#include <stdint.h>

#include "framebuffer.h"
#include "font8x16.h"

#define CONSOLE_FG 0x00E0E0E0u
#define CONSOLE_BG 0x00101018u
#define TAB_STOP   8u

static uint32_t cols;
static uint32_t rows;
static uint32_t cur_col;
static uint32_t cur_row;

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
            fb_put_pixel(base_x + x, base_y + y, rgb);
        }
    }
}

static void newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= rows) {
        fb_scroll_up(FONT_HEIGHT, CONSOLE_BG);
        cur_row = rows - 1;
    }
}

void console_init(void) {
    cols = fb_width() / FONT_WIDTH;
    rows = fb_height() / FONT_HEIGHT;
    cur_col = 0;
    cur_row = 0;
    fb_clear(CONSOLE_BG);
}

void console_putc(char c) {
    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            draw_glyph(cur_col, cur_row, ' ');
        }
        return;
    }
    if (c == '\t') {
        uint32_t next = ((cur_col / TAB_STOP) + 1) * TAB_STOP;
        while (cur_col < next && cur_col < cols) {
            draw_glyph(cur_col, cur_row, ' ');
            cur_col++;
        }
        if (cur_col >= cols) {
            newline();
        }
        return;
    }

    draw_glyph(cur_col, cur_row, c);
    cur_col++;
    if (cur_col >= cols) {
        newline();
    }
}

void console_puts(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}
