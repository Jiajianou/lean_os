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

#define HID_TOUCHPAD_MAX_CONTACTS 5

#define HID_TOUCHPAD_INPUT_MODE_MOUSE    0
#define HID_TOUCHPAD_INPUT_MODE_TOUCHPAD 3

typedef struct {
    uint16_t offset;
    uint8_t bits;
    uint8_t present;
} hid_field_t;

typedef struct {
    hid_field_t tip;
    hid_field_t confidence;
    hid_field_t contact_id;
    hid_field_t x;
    hid_field_t y;
} hid_touchpad_finger_layout_t;

typedef struct {
    uint8_t report_id;
    uint8_t finger_count;
    uint16_t report_bits;
    hid_touchpad_finger_layout_t fingers[HID_TOUCHPAD_MAX_CONTACTS];
    hid_field_t contact_count;
    hid_field_t button;
    hid_field_t scan_time;
    int32_t x_minimum;
    int32_t x_maximum;
    int32_t y_minimum;
    int32_t y_maximum;
    uint32_t width_tenths_mm;
    uint32_t height_tenths_mm;
    uint8_t input_mode_report_id;
    uint16_t input_mode_report_bits;
    hid_field_t input_mode;
    uint8_t switches_report_id;
    uint16_t switches_report_bits;
    hid_field_t surface_switch;
    hid_field_t button_switch;
} hid_touchpad_layout_t;

typedef struct {
    uint8_t touching;
    uint8_t confident;
    uint8_t id;
    int32_t x;
    int32_t y;
} hid_touchpad_contact_t;

typedef struct {
    uint8_t has_contact_count;
    uint8_t contact_count;
    uint8_t slots;
    uint8_t button;
    hid_touchpad_contact_t contacts[HID_TOUCHPAD_MAX_CONTACTS];
} hid_touchpad_report_t;

int hid_report_find_touchpad(const uint8_t *descriptor, uint32_t length, hid_touchpad_layout_t *out);

int hid_touchpad_decode(const hid_touchpad_layout_t *layout, const uint8_t *report, uint32_t length,
                        hid_touchpad_report_t *out);

uint32_t hid_touchpad_build_input_mode(const hid_touchpad_layout_t *layout, uint8_t mode, uint8_t *out,
                                       uint32_t capacity);

uint32_t hid_touchpad_build_switches(const hid_touchpad_layout_t *layout, uint8_t *out, uint32_t capacity);
