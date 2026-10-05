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
   back to the old copy rather than overrunning it.

   M225 (log-crash-path). The redraw was the whole screen inside the newline
   that scrolled, and the log draws the console with interrupts off: on the
   ThinkPad, which has no COM port, the longest interrupts-off stretch the
   log made was that one character - every cell of the panel repainted, 4.4
   ms measured at 4096x2160 under hvf. Now the newline only moves the grid,
   which is a ring of rows (`top` is the grid row on screen row 0), and the
   screen is repainted from it by console_repaint_step, a few cells per
   call, by the log's drain between its sections. Until the repaint has
   reached a cell, what is on the screen there is what was there before the
   scroll; a character written to such a cell goes into the grid only, and
   reaches the screen with the repaint. */
#define CONSOLE_GRID_MAX (512u * 192u)

static char grid[CONSOLE_GRID_MAX];
static int grid_usable;
static uint32_t cols;
static uint32_t rows;
static uint32_t top;
static uint32_t current_col;
static uint32_t current_row;
/* Cells in screen order (row * cols + col) the screen has been repainted
   up to since the last scroll; rows * cols when no repaint is owed. */
static uint32_t repaint_at;
static uint64_t cells_drawn;

static char *grid_cell(uint32_t col, uint32_t row) {
    return &grid[((top + row) % rows) * cols + col];
}

static void paint_glyph(uint32_t col, uint32_t row, uint8_t code) {
    cells_drawn++;
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

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    uint8_t code = (uint8_t)c;
    if (code > 0x7F) {
        code = '?';
    }
    if (grid_usable) {
        *grid_cell(col, row) = (char)code;
        if (row * cols + col >= repaint_at) {
            return;
        }
    }
    paint_glyph(col, row, code);
}

static void newline(void) {
    current_col = 0;
    current_row++;
    if (current_row < rows) {
        return;
    }
    if (!grid_usable) {
        framebuffer_scroll_up(FONT_HEIGHT, CONSOLE_BG);
        cells_drawn += (uint64_t)rows * cols;
        current_row = rows - 1;
        return;
    }
    /* The top `shift` rows of the screen leave it and their grid rows come
       back, blank, as the bottom ones. Memory only; the screen is owed a
       repaint from the top. */
    uint32_t shift = rows / 4u ? rows / 4u : 1u;
    for (uint32_t row = 0; row < shift; row++) {
        char *line = grid_cell(0, row);
        for (uint32_t col = 0; col < cols; col++) {
            line[col] = ' ';
        }
    }
    top = (top + shift) % rows;
    current_row = rows - shift;
    repaint_at = 0;
}

int console_repaint_pending(void) {
    return grid_usable && repaint_at < rows * cols;
}

/* One piece: the next glyph, or the run of blank cells from here to the
   next glyph or the row's end (at most `max_cells`) as ONE fill of the
   run's width - most of a boot log's screen is the blank tails of short
   lines, which M200's full redraw filled a row at a time and a cell-by-cell
   repaint filled 128 pixels at a time. Each cell of a run counts as one. A
   piece is never more than one glyph, so a caller reading the clock between
   pieces overruns its budget by one glyph's pixels at most. */
uint32_t console_repaint_step(uint32_t max_cells) {
    if (!grid_usable || max_cells == 0 || repaint_at >= rows * cols) {
        return 0;
    }
    uint32_t row = repaint_at / cols;
    uint32_t col = repaint_at % cols;
    const char *line = grid_cell(0, row);
    if (line[col] != ' ') {
        paint_glyph(col, row, (uint8_t)line[col]);
        repaint_at++;
        return 1;
    }
    uint32_t run = 1;
    while (col + run < cols && run < max_cells && line[col + run] == ' ') {
        run++;
    }
    framebuffer_fill_rect(col * FONT_WIDTH, row * FONT_HEIGHT, run * FONT_WIDTH, FONT_HEIGHT,
                          CONSOLE_BG);
    cells_drawn += run;
    repaint_at += run;
    return run;
}

int console_has_grid(void) {
    return grid_usable;
}

uint64_t console_cells_drawn(void) {
    return cells_drawn;
}

void console_init(void) {
    cols = framebuffer_width() / FONT_WIDTH;
    rows = framebuffer_height() / FONT_HEIGHT;
    grid_usable = cols * rows <= CONSOLE_GRID_MAX && rows > 0;
    top = 0;
    if (grid_usable) {
        for (uint32_t i = 0; i < cols * rows; i++) {
            grid[i] = ' ';
        }
    }
    repaint_at = rows * cols;
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
