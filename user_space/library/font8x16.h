#pragma once

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

#define FONT_CAP_TOP   2
#define FONT_X_TOP     5
#define FONT_BASELINE  12
#define FONT_DESC_LAST 14

#define FONT_GLYPH_COLS 7

extern const uint8_t font8x16[128][16];

extern const uint8_t font8x16_bold[128][16];
