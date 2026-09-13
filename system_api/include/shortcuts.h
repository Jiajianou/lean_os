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
    SHORTCUT_WORKSPACE_PREV,
    SHORTCUT_WORKSPACE_NEXT,
    SHORTCUT_WINDOW_TO_PREV,
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
    {SHORTCUT_CYCLE_BACKWARD, KBD_MOD_ALT | KBD_MOD_SHIFT, 0,            '\t',                 "Shift+Alt+Tab",  "Previous window"},
    {SHORTCUT_CYCLE_FORWARD,  KBD_MOD_ALT,                 KBD_MOD_SHIFT, '\t',                "Alt+Tab",        "Next window"},
    {SHORTCUT_LAUNCHER,       KBD_MOD_CTRL,                0,            ' ',                  "Ctrl+Space",     "Open the launcher"},
    {SHORTCUT_TASK_MANAGER,   KBD_MOD_CTRL | KBD_MOD_SHIFT, 0,           27,                   "Ctrl+Shift+Esc", "Task manager"},
    {SHORTCUT_CLOSE_WINDOW,   KBD_MOD_ALT,                 0,            (char)KBD_KEY_FN(4),  "Alt+F4",         "Close window"},
    {SHORTCUT_WINDOW_TO_PREV, KBD_MOD_CTRL | KBD_MOD_SHIFT | KBD_MOD_ALT, 0, (char)KBD_KEY_LEFT,  "Ctrl+Shift+Alt+Left",  "Window to previous desktop"},
    {SHORTCUT_WINDOW_TO_NEXT, KBD_MOD_CTRL | KBD_MOD_SHIFT | KBD_MOD_ALT, 0, (char)KBD_KEY_RIGHT, "Ctrl+Shift+Alt+Right", "Window to next desktop"},
    {SHORTCUT_WORKSPACE_PREV, KBD_MOD_CTRL | KBD_MOD_SHIFT, KBD_MOD_ALT,   (char)KBD_KEY_LEFT,  "Ctrl+Shift+Left",      "Previous desktop"},
    {SHORTCUT_WORKSPACE_NEXT, KBD_MOD_CTRL | KBD_MOD_SHIFT, KBD_MOD_ALT,   (char)KBD_KEY_RIGHT, "Ctrl+Shift+Right",     "Next desktop"},
    {SHORTCUT_SNAP_LEFT,      KBD_MOD_CTRL | KBD_MOD_ALT,  KBD_MOD_SHIFT, (char)KBD_KEY_LEFT,   "Ctrl+Alt+Left",  "Snap left"},
    {SHORTCUT_SNAP_RIGHT,     KBD_MOD_CTRL | KBD_MOD_ALT,  KBD_MOD_SHIFT, (char)KBD_KEY_RIGHT,  "Ctrl+Alt+Right", "Snap right"},
    {SHORTCUT_MAXIMIZE,       KBD_MOD_CTRL | KBD_MOD_ALT,  KBD_MOD_SHIFT, (char)KBD_KEY_UP,     "Ctrl+Alt+Up",    "Maximize"},
    {SHORTCUT_MINIMIZE,       KBD_MOD_CTRL | KBD_MOD_ALT,  KBD_MOD_SHIFT, (char)KBD_KEY_DOWN,   "Ctrl+Alt+Down",  "Minimize"},
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
