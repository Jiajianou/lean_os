#pragma once

#include <stdint.h>

#define SCANCODE_EXT_KEYPAD_ENTER 0x1C
#define SCANCODE_EXT_RIGHT_CTRL   0x1D
#define SCANCODE_EXT_KEYPAD_SLASH 0x35
#define SCANCODE_EXT_RIGHT_ALT    0x38
#define SCANCODE_EXT_HOME         0x47
#define SCANCODE_EXT_PAGE_UP      0x49
#define SCANCODE_EXT_END          0x4F
#define SCANCODE_EXT_PAGE_DOWN    0x51
#define SCANCODE_EXT_DELETE       0x53
#define SCANCODE_EXT_LEFT_GUI     0x5B
#define SCANCODE_EXT_RIGHT_GUI    0x5C

typedef struct {
    uint8_t held;
    uint8_t alone;
} keyboard_super_t;

char keyboard_extended_key(uint8_t code);

void keyboard_super_down(keyboard_super_t *state);

void keyboard_super_other_key(keyboard_super_t *state);

int keyboard_super_up(keyboard_super_t *state);
