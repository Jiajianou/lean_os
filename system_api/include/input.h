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

/* M216: the keys a laptop keyboard has beyond letters and arrows. Their
   codes are control characters the terminal's line discipline gives no
   meaning to (it takes 3, 4, 21, 26, 28 and 127), so a program that has
   never heard of them sees nothing it would act on. KEYBOARD_KEY_SUPER is
   the Windows key pressed and let go with nothing else - what opens the
   Start menu - and the desktop keeps it to itself. */
#define KEYBOARD_KEY_HOME      5
#define KEYBOARD_KEY_END       6
#define KEYBOARD_KEY_DELETE    7
#define KEYBOARD_KEY_PAGE_UP   11
#define KEYBOARD_KEY_PAGE_DOWN 12
#define KEYBOARD_KEY_SUPER     31

#define KEYBOARD_KEY_F1  14
#define KEYBOARD_KEY_F12 25
#define KEYBOARD_KEY_FUNCTION(n) (KEYBOARD_KEY_F1 + (n) - 1)

#ifdef __cplusplus
}
#endif
