#pragma once

#include <stdint.h>

#include "keyboard_keys.h"

typedef struct {
    uint8_t last_keys[8];
    uint8_t last_buttons;
    int have_last;
    keyboard_super_t super_key;
} usb_hid_state_t;

typedef struct {
    char ch[6];
    int mods[6];
    int count;
} usb_hid_keys_t;

void usb_hid_decode_keyboard(usb_hid_state_t *state, const uint8_t report[8],
                             usb_hid_keys_t *out);

typedef struct {
    int32_t dx, dy, wheel;
    uint8_t buttons;
    int deliver;
} usb_hid_mouse_t;

void usb_hid_decode_mouse(usb_hid_state_t *state, const uint8_t *report,
                          uint8_t report_length, usb_hid_mouse_t *out);

extern const char usb_hid_ascii[104];
extern const char usb_hid_ascii_shift[104];
