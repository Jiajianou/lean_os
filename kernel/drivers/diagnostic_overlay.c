#include "diagnostic_overlay.h"

#include "drivers/font8x16.h"
#include "drivers/framebuffer.h"

#define OVERLAY_COLUMN_CHARACTERS 80
#define OVERLAY_BACKGROUND 0x00101018u
#define OVERLAY_INK        0x00FFFFFFu

static uint32_t overlay_scale;
static uint32_t overlay_column;
static uint32_t overlay_row;
static uint32_t overlay_rows;
static uint32_t overlay_columns;

static void overlay_draw_character(uint32_t x, uint32_t y, char c) {
    unsigned char ch = (unsigned char)c;
    if (ch > 0x7Eu || ch < 0x20u) {
        ch = '?';
    }
    const uint8_t *glyph = font8x16[ch];
    for (uint32_t row = 0; row < FONT_HEIGHT * overlay_scale; row++) {
        uint8_t bits = glyph[row / overlay_scale];
        for (uint32_t col = 0; col < FONT_WIDTH * overlay_scale; col++) {
            if (bits & (0x80u >> (col / overlay_scale))) {
                framebuffer_put_pixel(x + col, y + row, OVERLAY_INK);
            }
        }
    }
}

void diagnostic_overlay_begin(void) {
    uint32_t width = framebuffer_width();
    uint32_t height = framebuffer_height();
    overlay_scale = (width >= 2560) ? 2 : 1;
    uint32_t cell_width = FONT_WIDTH * overlay_scale;
    uint32_t cell_height = FONT_HEIGHT * overlay_scale;
    overlay_rows = height / cell_height;
    overlay_columns = width / (cell_width * OVERLAY_COLUMN_CHARACTERS);
    if (overlay_columns == 0) {
        overlay_columns = 1;
    }
    overlay_column = 0;
    overlay_row = 0;
    if (width && height) {
        framebuffer_fill_rect(0, 0, width, height, OVERLAY_BACKGROUND);
    }
}

void diagnostic_overlay_line(const char *text) {
    if (overlay_rows == 0 || overlay_column >= overlay_columns) {
        return;
    }
    uint32_t cell_width = FONT_WIDTH * overlay_scale;
    uint32_t cell_height = FONT_HEIGHT * overlay_scale;
    uint32_t x = overlay_column * OVERLAY_COLUMN_CHARACTERS * cell_width;
    uint32_t y = overlay_row * cell_height;
    uint32_t limit = framebuffer_width();
    for (uint32_t i = 0; text[i] && i < OVERLAY_COLUMN_CHARACTERS - 1; i++) {
        if (x + cell_width > limit) {
            break;
        }
        overlay_draw_character(x, y, text[i]);
        x += cell_width;
    }
    overlay_row++;
    if (overlay_row >= overlay_rows) {
        overlay_row = 0;
        overlay_column++;
    }
}
