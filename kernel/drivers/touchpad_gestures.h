#pragma once

#include <stdint.h>

#include "drivers/hid_report.h"

#define TOUCHPAD_COUNTS_PER_MM 12
#define TOUCHPAD_SCROLL_TENTHS_MM_PER_DETENT 25
#define TOUCHPAD_TAP_MAXIMUM_MS 200
#define TOUCHPAD_TAP_MAXIMUM_TRAVEL_TENTHS_MM 30
#define TOUCHPAD_DEFAULT_WIDTH_TENTHS_MM 1000
#define TOUCHPAD_DEFAULT_HEIGHT_TENTHS_MM 600

#define TOUCHPAD_MAX_EVENTS 3

typedef struct {
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    uint8_t buttons;
    uint8_t tap;
} touchpad_event_t;

typedef struct {
    uint8_t id;
    int32_t x;
    int32_t y;
} touchpad_point_t;

typedef struct {
    uint32_t x_range;
    uint32_t y_range;
    uint32_t width_tenths_mm;
    uint32_t height_tenths_mm;

    uint8_t frame_expected;
    uint8_t frame_collected;
    uint8_t frame_button;
    uint8_t frame_points;
    touchpad_point_t frame[HID_TOUCHPAD_MAX_CONTACTS];

    uint8_t previous_count;
    touchpad_point_t previous[HID_TOUCHPAD_MAX_CONTACTS];

    int64_t remainder_x;
    int64_t remainder_y;
    int64_t remainder_scroll;

    uint8_t touching;
    uint32_t touch_started_ms;
    uint8_t touch_most_fingers;
    uint32_t touch_travel_tenths_mm;
    uint8_t touch_clicked;
    uint8_t touch_scrolled;

    uint8_t button_held;
    uint8_t button_mask;
    uint8_t buttons_out;
} touchpad_gestures_t;

void touchpad_gestures_init(touchpad_gestures_t *state, const hid_touchpad_layout_t *layout);

int touchpad_gestures_report(touchpad_gestures_t *state, const hid_touchpad_report_t *report, uint32_t now_ms,
                             touchpad_event_t *out);

int touchpad_gestures_frame(touchpad_gestures_t *state, const touchpad_point_t *points, uint8_t count,
                            uint8_t button, uint32_t now_ms, touchpad_event_t *out);
