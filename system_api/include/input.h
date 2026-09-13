#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t dx;
    int32_t dy;
    uint8_t buttons;
    uint32_t time_ms;
    int32_t wheel;
} mouse_event_t;

#define KBD_MOD_CTRL  1
#define KBD_MOD_ALT   2
#define KBD_MOD_SHIFT 4

#define KBD_KEY_UP    1
#define KBD_KEY_DOWN  2
#define KBD_KEY_LEFT  3
#define KBD_KEY_RIGHT 4

#define KBD_KEY_F1  14
#define KBD_KEY_F12 25
#define KBD_KEY_FN(n) (KBD_KEY_F1 + (n) - 1)

#ifdef __cplusplus
}
#endif
