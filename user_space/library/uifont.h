#pragma once

#include <stdint.h>

#define UI_FONT_MAX_COLS 16
#define UI_FONT_MAX_ROWS 24

typedef struct {
    uint8_t height;
    uint8_t cap_top;
    uint8_t x_top;
    uint8_t baseline;
    uint8_t desc_last;
    uint8_t max_advance;
    const uint16_t *rows;
    const uint16_t *rows_bold;
    const uint8_t  *advance;
    const uint8_t  *width;
} ui_font_t;

extern const ui_font_t ui_font_small;
extern const ui_font_t ui_font_ui;
extern const ui_font_t ui_font_large;

#define UI_FONT_SMALL_HEIGHT 12
#define UI_FONT_UI_HEIGHT    16
#define UI_FONT_LARGE_HEIGHT 24

#define UI_G_ARROW_LEFT  '\x01'
#define UI_G_ARROW_RIGHT '\x02'
#define UI_G_ARROW_UP    '\x03'
#define UI_G_ARROW_DOWN  '\x04'
#define UI_G_CHECK       '\x05'
#define UI_G_BULLET      '\x06'
#define UI_G_ELLIPSIS    '\x07'
#define UI_G_CLOSE       '\x08'

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
