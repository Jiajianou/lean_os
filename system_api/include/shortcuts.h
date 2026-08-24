/* system_api/include/shortcuts.h
 *
 * M49: the window-manager keyboard chords, in one table.
 *
 * There are two consumers and they must not drift: compositor.c's
 * handle_keyboard dispatches from this table, and settings.c's Shortcuts
 * pane lists it. A hand-maintained second copy of "what the chords are"
 * would be wrong within two milestones - the plan for this milestone said
 * so explicitly, and it is right: the whole reason a shortcuts pane is
 * worth having is that it tells the truth.
 *
 * The table is the dispatch, not a description of it. Adding a chord
 * means adding a row here and a case in the compositor's switch; there is
 * no way to add one that the settings window then doesn't know about.
 */
#pragma once

#include <stdint.h>

#include "input.h" /* KBD_MOD_*, KBD_KEY_* - the chords are expressed in exactly what the keyboard driver delivers */

typedef enum {
    SHORTCUT_NONE = 0,
    SHORTCUT_CYCLE_FORWARD,   /* Alt+Tab */
    SHORTCUT_CYCLE_BACKWARD,  /* Shift+Alt+Tab */
    SHORTCUT_LAUNCHER,        /* Ctrl+Space */
    SHORTCUT_TASK_MANAGER,    /* Ctrl+Shift+Esc */
    SHORTCUT_CLOSE_WINDOW,    /* Alt+F4 */
    SHORTCUT_SNAP_LEFT,       /* Ctrl+Alt+Left */
    SHORTCUT_SNAP_RIGHT,      /* Ctrl+Alt+Right */
    SHORTCUT_MAXIMIZE,        /* Ctrl+Alt+Up */
    SHORTCUT_MINIMIZE,        /* Ctrl+Alt+Down */
} shortcut_id_t;

typedef struct {
    uint8_t id;       /* shortcut_id_t */
    uint8_t mods;     /* the KBD_MOD_* bits that must ALL be held */
    uint8_t mods_forbidden; /* ...and the ones that must NOT be, so Alt+Tab and Shift+Alt+Tab stay distinct */
    char ch;          /* the character the keyboard driver delivers for this key */
    const char *chord; /* what a person calls it - the Shortcuts pane's left column */
    const char *what;  /* ...and its right column */
} shortcut_t;

/* Order matters only for display; the compositor matches on (ch, mods)
 * and takes the first row that fits, so the more specific chord has to
 * come first where two share a key. Shift+Alt+Tab is exactly that case
 * and is listed above plain Alt+Tab for it. */
static const shortcut_t SHORTCUTS[] = {
    {SHORTCUT_CYCLE_BACKWARD, KBD_MOD_ALT | KBD_MOD_SHIFT, 0,            '\t',                 "Shift+Alt+Tab",  "Previous window"},
    {SHORTCUT_CYCLE_FORWARD,  KBD_MOD_ALT,                 KBD_MOD_SHIFT, '\t',                "Alt+Tab",        "Next window"},
    {SHORTCUT_LAUNCHER,       KBD_MOD_CTRL,                0,            ' ',                  "Ctrl+Space",     "Open the launcher"},
    {SHORTCUT_TASK_MANAGER,   KBD_MOD_CTRL | KBD_MOD_SHIFT, 0,           27,                   "Ctrl+Shift+Esc", "Task manager"},
    {SHORTCUT_CLOSE_WINDOW,   KBD_MOD_ALT,                 0,            (char)KBD_KEY_FN(4),  "Alt+F4",         "Close window"},
    {SHORTCUT_SNAP_LEFT,      KBD_MOD_CTRL | KBD_MOD_ALT,  0,            (char)KBD_KEY_LEFT,   "Ctrl+Alt+Left",  "Snap left"},
    {SHORTCUT_SNAP_RIGHT,     KBD_MOD_CTRL | KBD_MOD_ALT,  0,            (char)KBD_KEY_RIGHT,  "Ctrl+Alt+Right", "Snap right"},
    {SHORTCUT_MAXIMIZE,       KBD_MOD_CTRL | KBD_MOD_ALT,  0,            (char)KBD_KEY_UP,     "Ctrl+Alt+Up",    "Maximize"},
    {SHORTCUT_MINIMIZE,       KBD_MOD_CTRL | KBD_MOD_ALT,  0,            (char)KBD_KEY_DOWN,   "Ctrl+Alt+Down",  "Minimize"},
};

#define SHORTCUT_COUNT ((int)(sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0])))

/* Which shortcut (if any) `ch` with `mods` held is - SHORTCUT_NONE for an
 * ordinary keystroke, which is the overwhelmingly common answer and the
 * one that has to stay cheap. A static inline for the same reason
 * spawn_error_message is one: it is a short loop over a table both sides
 * already include, and giving it a translation unit on each side would be
 * more machinery than the thing itself. */
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
