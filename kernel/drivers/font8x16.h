/* kernel/drivers/font8x16.h
 *
 * See font8x16.c's header comment for how this data was produced.
 */
#pragma once

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/* One glyph per ASCII codepoint 0x00-0x7F, 16 rows of 8 pixels each (one
 * byte per row, MSB = leftmost pixel). Codepoints outside 0x20-0x7E
 * (control codes, 0x7F) are blank. */
extern const uint8_t font8x16[128][16];
