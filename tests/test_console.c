#include "check.h"

#include "drivers/console.h"
#include "drivers/font8x16.h"

#include <stdio.h>
#include <string.h>

void fake_framebuffer_reset(uint32_t w, uint32_t h);
unsigned long fake_framebuffer_reads(void);
unsigned long fake_framebuffer_scrolls(void);
unsigned long long fake_framebuffer_writes(void);
unsigned long long fake_framebuffer_calls(void);
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

/* M225: a scroll leaves the screen owed a repaint, which whoever feeds the
   console - the log's drain - pays a few cells at a time. */
static void finish_repaint(void) {
    while (console_repaint_step(128)) {
    }
}

TEST(console, what_is_on_the_screen_after_scrolling_is_the_last_lines_in_order) {
    fake_framebuffer_reset(640, 480);
    console_init();
    char line[64];
    for (int i = 0; i < 500; i++) {
        snprintf(line, sizeof(line), "line %d of the boot log\n", i);
        console_puts(line);
    }
    finish_repaint();
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

/* M225 (log-crash-path). The log draws the console a character at a time
   with interrupts off, and on the ThinkPad - no COM port - that is all the
   log's devices are. The character that scrolled used to repaint the whole
   screen from the grid inside console_putc: on the 3840x2400 panel, nine
   million pixels in one interrupts-off section, every 37 lines. No
   character may paint more than a tab's eight cells now. */
TEST(console, no_character_paints_more_than_a_tab_on_the_4k_panel) {
    fake_framebuffer_reset(3840, 2400);
    console_init();
    unsigned long long most = 0;
    char line[64];
    for (int i = 0; i < 400; i++) {
        snprintf(line, sizeof(line), "line %d\tof the boot log\n", i);
        for (const char *p = line; *p; p++) {
            unsigned long long before = fake_framebuffer_writes();
            console_putc(*p);
            unsigned long long painted = fake_framebuffer_writes() - before;
            if (painted > most) {
                most = painted;
            }
        }
    }
    CHECK_MSG(most <= 8ull * FONT_WIDTH * FONT_HEIGHT,
              "one character painted %llu pixels", most);
}

/* M225: a scroll leaves the screen owed a repaint, paid in pieces no bigger
   than asked for - and when it has been paid the screen is what finishing
   every repaint at once would have made, even with characters written in
   the middle of it. */
static uint32_t reference[1024 * 768];

static void write_boot_log(int lines, int steps_between) {
    char line[64];
    for (int i = 0; i < lines; i++) {
        snprintf(line, sizeof(line), "line %d of the boot log%s\n", i, i % 7 ? "" : "\tand a tab");
        console_puts(line);
        if (steps_between < 0) {
            finish_repaint();
        }
        for (int s = 0; s < steps_between && console_repaint_pending(); s++) {
            console_repaint_step(37);
        }
    }
}

TEST(console, a_scroll_is_repainted_in_pieces_and_ends_as_a_full_redraw_would) {
    fake_framebuffer_reset(1024, 768);
    console_init();
    write_boot_log(300, -1);
    CHECK_EQ(console_repaint_pending(), 0);
    for (uint32_t y = 0; y < 768; y++) {
        for (uint32_t x = 0; x < 1024; x++) {
            reference[y * 1024 + x] = fake_framebuffer_pixel(x, y);
        }
    }

    fake_framebuffer_reset(1024, 768);
    console_init();
    write_boot_log(300, 3);
    int pieces = 0;
    while (console_repaint_pending()) {
        unsigned long long before = fake_framebuffer_writes();
        uint32_t done = console_repaint_step(37);
        CHECK(done > 0 && done <= 37);
        CHECK(fake_framebuffer_writes() - before <= 37ull * FONT_WIDTH * FONT_HEIGHT);
        pieces++;
    }
    CHECK_EQ(console_repaint_step(37), 0u);
    CHECK(pieces > 0);
    unsigned long differ = 0;
    for (uint32_t y = 0; y < 768; y++) {
        for (uint32_t x = 0; x < 1024; x++) {
            differ += reference[y * 1024 + x] != fake_framebuffer_pixel(x, y);
        }
    }
    CHECK_EQ(differ, 0ul);
}

/* A repaint paid a cell at a time cost a fill per blank cell, where M200's
   full redraw filled a row's blank tail in one: on the 4K panel, a boot
   log's screen is mostly those tails, and the repaint made ~60,000 calls
   for what had been ~150 fills and the glyphs. A run of blanks is one fill
   again - in pieces as small as the log's step - and the screen it leaves
   is pixel for pixel the same (the test above). */
TEST(console, a_run_of_blank_cells_is_one_fill) {
    fake_framebuffer_reset(3840, 2400);
    console_init();
    const uint32_t rows = 2400 / FONT_HEIGHT;
    char line[64];
    for (uint32_t i = 0; i < rows; i++) {
        snprintf(line, sizeof(line), "[boot] line %u\n", i);
        console_puts(line);
    }
    CHECK(console_repaint_pending());
    unsigned long long before = fake_framebuffer_calls();
    uint64_t cells_before = console_cells_drawn();
    uint32_t steps = 0;
    unsigned long long most_in_a_step = 0;
    while (console_repaint_pending()) {
        unsigned long long at = fake_framebuffer_calls();
        CHECK(console_repaint_step(16) <= 16u);
        if (fake_framebuffer_calls() - at > most_in_a_step) {
            most_in_a_step = fake_framebuffer_calls() - at;
        }
        steps++;
    }
    /* A piece is one glyph or one fill - so a caller reading the clock
       between pieces overruns its budget by a glyph at most. */
    CHECK(most_in_a_step <= (unsigned long long)FONT_WIDTH * FONT_HEIGHT);
    uint64_t cells = console_cells_drawn() - cells_before;
    CHECK_EQ(cells, (uint64_t)(3840 / FONT_WIDTH) * rows);
    /* Every glyph's pixels, plus a fill per run of blanks - a line's two
       inner spaces and its tail - and one more each time a step's limit
       cut a run. */
    uint64_t glyph_cells = 0;
    for (uint32_t r = 0; r < rows; r++) {
        char text[24];
        row_text(r, text, sizeof(text));
        for (const char *p = text; *p; p++) {
            glyph_cells += *p != ' ';
        }
    }
    unsigned long long calls = fake_framebuffer_calls() - before;
    CHECK_MSG(calls <= glyph_cells * FONT_WIDTH * FONT_HEIGHT + 3ull * rows + steps,
              "%llu framebuffer calls for %llu glyph cells in %u steps", calls,
              (unsigned long long)glyph_cells, steps);
}

TEST(console, a_screen_with_more_cells_than_the_grid_says_so) {
    fake_framebuffer_reset(3840, 2400);
    console_init();
    CHECK_EQ(console_has_grid(), 1);
    fake_framebuffer_reset(5120, 2880);
    console_init();
    CHECK_EQ(console_has_grid(), 0);
}

TEST(console, cells_drawn_counts_what_was_painted) {
    fake_framebuffer_reset(640, 480);
    console_init();
    uint64_t before = console_cells_drawn();
    console_puts("ab\t");
    CHECK_EQ(console_cells_drawn() - before, (uint64_t)(2 + 6));
}
