#include "check.h"

#include "drivers/console.h"
#include "drivers/font8x16.h"

#include <stdio.h>
#include <string.h>

void fake_framebuffer_reset(uint32_t w, uint32_t h);
unsigned long fake_framebuffer_reads(void);
unsigned long fake_framebuffer_scrolls(void);
uint32_t fake_framebuffer_pixel(uint32_t x, uint32_t y);

#define CONSOLE_FG 0x00E0E0E0u

/* Which character is drawn in a cell, read back out of the pixels by
   matching them against the font - the screen, not the console's idea of it. */
static char cell_at(uint32_t col, uint32_t row) {
    for (int c = 32; c < 127; c++) {
        int same = 1;
        for (uint32_t y = 0; y < FONT_HEIGHT && same; y++) {
            for (uint32_t x = 0; x < FONT_WIDTH && same; x++) {
                int ink = (font8x16[c][y] & (0x80 >> x)) != 0;
                int lit = fake_framebuffer_pixel(col * FONT_WIDTH + x, row * FONT_HEIGHT + y) == CONSOLE_FG;
                same = ink == lit;
            }
        }
        if (same) {
            return (char)c;
        }
    }
    return '?';
}

static void row_text(uint32_t row, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) {
        out[i] = cell_at((uint32_t)i, row);
    }
    out[i] = 0;
    while (i > 0 && out[i - 1] == ' ') {
        out[--i] = 0;
    }
}

TEST(console, a_scrolling_console_never_reads_the_framebuffer) {
    fake_framebuffer_reset(640, 480);
    console_init();
    char line[64];
    for (int i = 0; i < 500; i++) {
        snprintf(line, sizeof(line), "line %d of the boot log\n", i);
        console_puts(line);
    }
    CHECK_EQ(fake_framebuffer_scrolls(), 0);
    CHECK_EQ(fake_framebuffer_reads(), 0);
}

TEST(console, what_is_on_the_screen_after_scrolling_is_the_last_lines_in_order) {
    fake_framebuffer_reset(640, 480);
    console_init();
    char line[64];
    for (int i = 0; i < 500; i++) {
        snprintf(line, sizeof(line), "line %d of the boot log\n", i);
        console_puts(line);
    }
    uint32_t rows = 480 / FONT_HEIGHT;
    int seen_last = -1;
    int previous = -1;
    int in_order = 1;
    for (uint32_t row = 0; row < rows; row++) {
        char text[80];
        row_text(row, text, sizeof(text));
        int n = -1;
        if (sscanf(text, "line %d of the boot log", &n) == 1) {
            if (previous >= 0 && n != previous + 1) {
                in_order = 0;
            }
            previous = n;
            seen_last = n;
        }
    }
    CHECK_EQ(seen_last, 499);
    CHECK_EQ(in_order, 1);
}

TEST(console, a_screen_too_big_for_the_grid_still_scrolls) {
    fake_framebuffer_reset(8 * 600, 16 * 200);
    console_init();
    for (int i = 0; i < 205; i++) {
        console_puts("x\n");
    }
    CHECK(fake_framebuffer_scrolls() > 0);
}
