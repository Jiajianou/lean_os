/* user_space/lib/font8x16.h
 *
 * GENERATED FILE - do not edit by hand.
 *
 * Produced by tools/gen-font.c (M39), which holds the one glyph
 * source both copies of this table come from and the metric checks
 * that keep them honest. Regenerate with `make font`; `make
 * font-check` (a prerequisite of the build) fails if this file
 * stops matching what that program produces.
 *
 * A byte-identical copy of kernel/drivers/font8x16.{h,c}'s glyph
 * tables - a user program can't link against the kernel image, so
 * this plain-data table (no kernel dependency at all) is duplicated
 * rather than shared, the same reasoning compositor.c's hand-authored
 * cursor sprite already used for the same kernel/user split.
 *
 * M39: the "keep the two tables in sync by hand" warning this header
 * used to carry is gone - both files now come out of tools/gen-font.c,
 * and the build refuses to proceed if they diverge. See gfx.h for the
 * user-space text-drawing API built on this.
 */
#pragma once

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/* M39's shared metric, in rows of the 16-row cell. Exposed so callers
 * that need to reason about text geometry (and the boot-time self-test
 * that verifies it) use the same numbers gen-font.c enforced. */
#define FONT_CAP_TOP   2  /* top of uppercase, digits, ascenders */
#define FONT_X_TOP     5  /* top of the lowercase x-height band */
#define FONT_BASELINE  12  /* first row *below* the glyph body */
#define FONT_DESC_LAST 14  /* bottom row of descenders (g j p q y) */

/* Every glyph lives in columns 0-6; column 7 is always blank - the
 * advance gap that gives text uniform letter spacing (text drawing
 * advances exactly FONT_WIDTH with no tracking of its own) and that
 * makes font8x16_bold below lossless. */
#define FONT_GLYPH_COLS 7

/* One glyph per ASCII codepoint 0x00-0x7F, 16 rows of 8 pixels each (one
 * byte per row, MSB = leftmost pixel). Codepoints outside 0x20-0x7E
 * (control codes, 0x7F) are blank. */
extern const uint8_t font8x16[128][16];

/* The bold weight: font8x16 dilated one column to the right. Computed
 * once at generation time, not per-pixel at draw time - and lossless,
 * because column 7 is reserved blank so nothing shifts off the end.
 * (M38 did this smear at draw time inside the byte, which silently
 * dropped the thickening of any glyph that already had ink in column
 * 7 - exactly the stems that most needed it.) */
extern const uint8_t font8x16_bold[128][16];
