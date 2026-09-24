#pragma once

#include <stdint.h>

#define HID_MOUSE_MAX_BUTTONS 8

typedef struct {
    uint8_t report_id;
    uint8_t has_report_id;
    uint16_t report_bits;
    uint16_t button_offset;
    uint8_t button_count;
    uint16_t x_offset;
    uint16_t y_offset;
    uint16_t wheel_offset;
    uint8_t x_bits;
    uint8_t y_bits;
    uint8_t wheel_bits;
    uint8_t has_x;
    uint8_t has_y;
    uint8_t has_wheel;
} hid_mouse_layout_t;

typedef struct {
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    uint8_t buttons;
} hid_mouse_report_t;

int hid_report_find_mouse(const uint8_t *descriptor, uint32_t length, hid_mouse_layout_t *out);

int hid_mouse_decode(const hid_mouse_layout_t *layout, const uint8_t *report, uint32_t length,
                     hid_mouse_report_t *out);
