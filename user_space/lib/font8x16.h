/* user_space/lib/font8x16.h
 *
 * M21: a verbatim copy of kernel/drivers/font8x16.{h,c}'s glyph table -
 * a user program can't link against the kernel image, so the plain-data
 * table (no kernel dependency at all - see that file's header comment)
 * gets duplicated here rather than shared, the same reasoning
 * compositor.c's hand-authored cursor sprite already used for the same
 * kernel/user split. Keep the two tables in sync by hand if either ever
 * changes; see gfx.h for the user-space text-drawing API built on this.
 */
#pragma once

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/* One glyph per ASCII codepoint 0x00-0x7F, 16 rows of 8 pixels each (one
 * byte per row, MSB = leftmost pixel). Codepoints outside 0x20-0x7E
 * (control codes, 0x7F) are blank. */
extern const uint8_t font8x16[128][16];
