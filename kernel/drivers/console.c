#include "console.h"

#include <stdint.h>

#include "framebuffer.h"
#include "font8x16.h"

#define CONSOLE_FG 0x00E0E0E0u
#define CONSOLE_BG 0x00101018u
#define TAB_STOP   8u

/* M200. Scrolling copied the framebuffer up a text row at a time, and the
   framebuffer is write-combining memory: every pixel of the screen was READ
   back through an uncached path to be written a row higher. On the laptop's
   3840x2400 panel that is 37 MB of uncached reads a line - the boot log's own
   timestamps were 25 ms apart, and a few hundred lines of it were several
   seconds of every boot. The console keeps its characters here instead,
   scrolls a quarter of the screen at a time, and redraws from them, so the
   framebuffer is only ever written. A screen with more cells than this falls
   back to the old copy rather than overrunning it. */
#define CONSOLE_GRID_MAX (512u * 192u)

static char grid[CONSOLE_GRID_MAX];
static int grid_usable;
static uint32_t cols;
static uint32_t rows;
static uint32_t current_col;
static uint32_t current_row;

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    uint8_t code = (uint8_t)c;
    if (code > 0x7F) {
        code = '?';
    }
    if (grid_usable) {
        grid[row * cols + col] = (char)code;
    }
    uint32_t base_x = col * FONT_WIDTH;
    uint32_t base_y = row * FONT_HEIGHT;
    if (code == ' ') {
        framebuffer_fill_rect(base_x, base_y, FONT_WIDTH, FONT_HEIGHT, CONSOLE_BG);
        return;
    }
    const uint8_t *glyph = font8x16[code];
    for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT_WIDTH; x++) {
            uint32_t rgb = (bits & (0x80 >> x)) ? CONSOLE_FG : CONSOLE_BG;
            framebuffer_put_pixel(base_x + x, base_y + y, rgb);
        }
    }
}

static void redraw_from_grid(void) {
    for (uint32_t row = 0; row < rows; row++) {
        uint32_t last = 0;
        for (uint32_t col = 0; col < cols; col++) {
            if (grid[row * cols + col] != ' ') {
                last = col + 1;
            }
        }
        for (uint32_t col = 0; col < last; col++) {
            draw_glyph(col, row, grid[row * cols + col]);
        }
        if (last < cols) {
            framebuffer_fill_rect(last * FONT_WIDTH, row * FONT_HEIGHT,
                                  (cols - last) * FONT_WIDTH, FONT_HEIGHT, CONSOLE_BG);
        }
    }
}

static void newline(void) {
    current_col = 0;
    current_row++;
    if (current_row < rows) {
        return;
    }
    if (!grid_usable) {
        framebuffer_scroll_up(FONT_HEIGHT, CONSOLE_BG);
        current_row = rows - 1;
        return;
    }
    uint32_t shift = rows / 4u ? rows / 4u : 1u;
    for (uint32_t i = 0; i < (rows - shift) * cols; i++) {
        grid[i] = grid[i + shift * cols];
    }
    for (uint32_t i = (rows - shift) * cols; i < rows * cols; i++) {
        grid[i] = ' ';
    }
    redraw_from_grid();
    current_row = rows - shift;
}

void console_init(void) {
    cols = framebuffer_width() / FONT_WIDTH;
    rows = framebuffer_height() / FONT_HEIGHT;
    grid_usable = cols * rows <= CONSOLE_GRID_MAX && rows > 0;
    if (grid_usable) {
        for (uint32_t i = 0; i < cols * rows; i++) {
            grid[i] = ' ';
        }
    }
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
