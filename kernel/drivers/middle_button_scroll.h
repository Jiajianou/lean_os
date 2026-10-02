#pragma once

#include <stdint.h>

#define MIDDLE_BUTTON_SCROLL_COUNTS_PER_DETENT 10
#define MIDDLE_BUTTON_SCROLL_STILL_COUNTS 3

typedef struct {
    uint8_t held;
    uint8_t scrolled;
    uint8_t buttons_out;
    int32_t remainder;
    int32_t travel;
} middle_button_scroll_t;

typedef struct {
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    uint8_t buttons;
} middle_button_scroll_event_t;

int middle_button_scroll_filter(middle_button_scroll_t *state, int32_t dx, int32_t dy, uint8_t buttons,
                                middle_button_scroll_event_t out[2]);
