#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t dx;
    int32_t dy;
    uint8_t buttons;
    uint8_t source;
    uint8_t flags;
    uint32_t time_ms;
    int32_t wheel;
} mouse_event_t;

#define MOUSE_SOURCE_MOUSE    0
#define MOUSE_SOURCE_TRACKPAD 1

#define MOUSE_FLAG_TAP 1

#define KEYBOARD_MOD_CTRL  1
#define KEYBOARD_MOD_ALT   2
#define KEYBOARD_MOD_SHIFT 4

#define KEYBOARD_KEY_UP    1
#define KEYBOARD_KEY_DOWN  2
#define KEYBOARD_KEY_LEFT  3
#define KEYBOARD_KEY_RIGHT 4

#define KEYBOARD_KEY_F1  14
#define KEYBOARD_KEY_F12 25
#define KEYBOARD_KEY_FUNCTION(n) (KEYBOARD_KEY_F1 + (n) - 1)

#ifdef __cplusplus
}
#endif
