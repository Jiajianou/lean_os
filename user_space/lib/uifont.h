/* user_space/lib/uifont.h
 *
 * GENERATED FILE - do not edit by hand.
 *
 * Produced by tools/gen-font.c (M57), which holds the glyph source
 * for all three sizes and the family checks that keep them on one
 * baseline. Regenerate with `make font`; `make font-check` (a
 * prerequisite of the build) fails if this file stops matching what
 * that program produces.
 *
 * M57: the proportional UI font family - per-glyph advance widths
 * beside the bitmap, in three sizes on one shared metric. This is what
 * chrome, labels, menus, buttons and filenames draw with; font8x16.h's
 * fixed cell stays exactly where it belongs, under gui_terminal.c's
 * character grid, text_editor.c's column arithmetic and the kernel
 * console.
 *
 * A row is a uint16_t, MSB = leftmost column, so a glyph may be up to
 * 16 columns wide. `width[c]` is the glyph's box and `advance[c]` is
 * how far the pen moves after drawing it - always at least one column
 * more, which is where letter spacing now comes from. Measure text with
 * gfx_text_width() (gfx.h); nothing may assume a constant advance.
 */
#pragma once

#include <stdint.h>

#define UI_FONT_MAX_COLS 16
#define UI_FONT_MAX_ROWS 24

typedef struct {
    uint8_t height;      /* rows in the cell - the line height */
    uint8_t cap_top;     /* first row of uppercase, digits, ascenders */
    uint8_t x_top;       /* first row of the lowercase x-height band */
    uint8_t baseline;    /* first row *below* the glyph body */
    uint8_t desc_last;   /* last row of descenders (g j p q y) */
    uint8_t max_advance; /* widest advance in the face */
    const uint16_t *rows;      /* 128 * height, MSB = leftmost column */
    const uint16_t *rows_bold; /* same shape, or NULL if this size has no bold */
    const uint8_t  *advance;   /* 128 */
    const uint8_t  *width;     /* 128 - the glyph box, always < advance */
} ui_font_t;

/* 12-row cell, 1px strokes - dense lists (file manager, task manager). */
extern const ui_font_t ui_font_small;
/* 16-row cell - chrome and labels. gfx.c's default. */
extern const ui_font_t ui_font_ui;
/* 24-row cell - headings and dialog titles. */
extern const ui_font_t ui_font_large;

/* Each face's line height as a constant, because window layout in this
 * project is compile-time arithmetic (#define ROW_H, WIN_H, ...) and a
 * struct field cannot appear in a #define. These are the same numbers
 * ui_font_*.height carries at runtime - emitted together so they cannot
 * drift. Widths have no equivalent and never will: that is the whole
 * point of this milestone. */
#define UI_FONT_SMALL_HEIGHT 12
#define UI_FONT_UI_HEIGHT    16
#define UI_FONT_LARGE_HEIGHT 24

/* The glyphs the UI used to fake with hand-drawn rectangles and
 * spelled-out words. They live in the control-code range, which is
 * blank in every string this OS draws, so nothing real collides with
 * them - write them straight into a literal: "Delete" UI_S_ELLIPSIS. */
#define UI_G_ARROW_LEFT  '\x01' /* left-pointing triangle */
#define UI_G_ARROW_RIGHT '\x02' /* right-pointing triangle (submenu, disclosure) */
#define UI_G_ARROW_UP    '\x03' /* up-pointing triangle (sort ascending) */
#define UI_G_ARROW_DOWN  '\x04' /* down-pointing triangle (sort descending) */
#define UI_G_CHECK       '\x05' /* checkmark */
#define UI_G_BULLET      '\x06' /* bullet / radio dot */
#define UI_G_ELLIPSIS    '\x07' /* ellipsis */
#define UI_G_CLOSE       '\x08' /* close X */

#define UI_S_ARROW_LEFT  "\x01"
#define UI_S_ARROW_RIGHT "\x02"
#define UI_S_ARROW_UP    "\x03"
#define UI_S_ARROW_DOWN  "\x04"
#define UI_S_CHECK       "\x05"
#define UI_S_BULLET      "\x06"
#define UI_S_ELLIPSIS    "\x07"
#define UI_S_CLOSE       "\x08"

#define UI_GLYPH_SPECIAL_FIRST 0x01
#define UI_GLYPH_SPECIAL_LAST  0x08
