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

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

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
    /* M63 stretch goal: virtual desktops, and the natural payoff for
     * this table existing. Ctrl+Shift+arrow moves *you*; adding Alt moves
     * the window with you - which is the one thing that makes a second
     * desktop useful for something already open rather than only for
     * something about to be started. */
    SHORTCUT_WORKSPACE_PREV,  /* Ctrl+Shift+Left */
    SHORTCUT_WORKSPACE_NEXT,  /* Ctrl+Shift+Right */
    SHORTCUT_WINDOW_TO_PREV,  /* Ctrl+Shift+Alt+Left */
    SHORTCUT_WINDOW_TO_NEXT,  /* Ctrl+Shift+Alt+Right */
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
    /* M63: the four-modifier chords come *before* the snap ones they
     * extend, for exactly the reason Shift+Alt+Tab comes before Alt+Tab -
     * the matcher takes the first row that fits. Getting this wrong is
     * not subtle to debug and was not subtle here: Ctrl+Shift+Alt+Right
     * matched "Snap right" and snapped the window instead of sending it
     * to the next desktop, which the self-test caught on the first boot.
     * The forbidden mask on the snap rows says the same thing a second
     * way, so a future reordering cannot reintroduce it. */
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

#ifdef __cplusplus
}
#endif
