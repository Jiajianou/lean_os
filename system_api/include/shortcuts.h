#pragma once

#include <stdint.h>

#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHORTCUT_NONE = 0,
    SHORTCUT_CYCLE_FORWARD,
    SHORTCUT_CYCLE_BACKWARD,
    SHORTCUT_LAUNCHER,
    SHORTCUT_TASK_MANAGER,
    SHORTCUT_CLOSE_WINDOW,
    SHORTCUT_SNAP_LEFT,
    SHORTCUT_SNAP_RIGHT,
    SHORTCUT_MAXIMIZE,
    SHORTCUT_MINIMIZE,
    SHORTCUT_WORKSPACE_PREVIOUS,
    SHORTCUT_WORKSPACE_NEXT,
    SHORTCUT_WINDOW_TO_PREVIOUS,
    SHORTCUT_WINDOW_TO_NEXT,
} shortcut_id_t;

typedef struct {
    uint8_t id;
    uint8_t mods;
    uint8_t mods_forbidden;
    char ch;
    const char *chord;
    const char *what;
} shortcut_t;

static const shortcut_t SHORTCUTS[] = {
    {SHORTCUT_CYCLE_BACKWARD, KEYBOARD_MOD_ALT | KEYBOARD_MOD_SHIFT, 0,            '\t',                 "Shift+Alt+Tab",  "Previous window"},
    {SHORTCUT_CYCLE_FORWARD,  KEYBOARD_MOD_ALT,                 KEYBOARD_MOD_SHIFT, '\t',                "Alt+Tab",        "Next window"},
    {SHORTCUT_LAUNCHER,       KEYBOARD_MOD_CTRL,                0,            ' ',                  "Ctrl+Space",     "Open the launcher"},
    {SHORTCUT_TASK_MANAGER,   KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT, 0,           27,                   "Ctrl+Shift+Esc", "Task manager"},
    {SHORTCUT_CLOSE_WINDOW,   KEYBOARD_MOD_ALT,                 0,            (char)KEYBOARD_KEY_FUNCTION(4),  "Alt+F4",         "Close window"},
    {SHORTCUT_WINDOW_TO_PREVIOUS, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT | KEYBOARD_MOD_ALT, 0, (char)KEYBOARD_KEY_LEFT,  "Ctrl+Shift+Alt+Left",  "Window to previous desktop"},
    {SHORTCUT_WINDOW_TO_NEXT, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT | KEYBOARD_MOD_ALT, 0, (char)KEYBOARD_KEY_RIGHT, "Ctrl+Shift+Alt+Right", "Window to next desktop"},
    {SHORTCUT_WORKSPACE_PREVIOUS, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT, KEYBOARD_MOD_ALT,   (char)KEYBOARD_KEY_LEFT,  "Ctrl+Shift+Left",      "Previous desktop"},
    {SHORTCUT_WORKSPACE_NEXT, KEYBOARD_MOD_CTRL | KEYBOARD_MOD_SHIFT, KEYBOARD_MOD_ALT,   (char)KEYBOARD_KEY_RIGHT, "Ctrl+Shift+Right",     "Next desktop"},
    {SHORTCUT_SNAP_LEFT,      KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT,  KEYBOARD_MOD_SHIFT, (char)KEYBOARD_KEY_LEFT,   "Ctrl+Alt+Left",  "Snap left"},
    {SHORTCUT_SNAP_RIGHT,     KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT,  KEYBOARD_MOD_SHIFT, (char)KEYBOARD_KEY_RIGHT,  "Ctrl+Alt+Right", "Snap right"},
    {SHORTCUT_MAXIMIZE,       KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT,  KEYBOARD_MOD_SHIFT, (char)KEYBOARD_KEY_UP,     "Ctrl+Alt+Up",    "Maximize"},
    {SHORTCUT_MINIMIZE,       KEYBOARD_MOD_CTRL | KEYBOARD_MOD_ALT,  KEYBOARD_MOD_SHIFT, (char)KEYBOARD_KEY_DOWN,   "Ctrl+Alt+Down",  "Minimize"},
};

#define SHORTCUT_COUNT ((int)(sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0])))

static inline int shortcut_lookup(char ch, int mods) {
    for (int i = 0; i < SHORTCUT_COUNT; i++) {
        if (SHORTCUTS[i].ch == ch &&
            (mods & SHORTCUTS[i].mods) == SHORTCUTS[i].mods &&
            (mods & SHORTCUTS[i].mods_forbidden) == 0) {
            return SHORTCUTS[i].id;
        }
    }
    return SHORTCUT_NONE;
}

#ifdef __cplusplus
}
#endif
