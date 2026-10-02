#include "keyboard_keys.h"

#include "input.h"

/* M216. After an 0xE0 the PS/2 keyboard names the keys a desktop keyboard
   doubled or added: the arrows were the only ones read, so Delete, Home,
   End and the page keys did nothing, and a right Ctrl or Alt held down was
   no modifier at all. What each press is, as the code a program receives;
   0 for a key that is a modifier or means nothing. The arrows' codes are
   the ones the driver always had. */
char keyboard_extended_key(uint8_t code) {
    switch (code) {
    case 0x48:
        return (char)KEYBOARD_KEY_UP;
    case 0x50:
        return (char)KEYBOARD_KEY_DOWN;
    case 0x4B:
        return (char)KEYBOARD_KEY_LEFT;
    case 0x4D:
        return (char)KEYBOARD_KEY_RIGHT;
    case SCANCODE_EXT_HOME:
        return (char)KEYBOARD_KEY_HOME;
    case SCANCODE_EXT_END:
        return (char)KEYBOARD_KEY_END;
    case SCANCODE_EXT_PAGE_UP:
        return (char)KEYBOARD_KEY_PAGE_UP;
    case SCANCODE_EXT_PAGE_DOWN:
        return (char)KEYBOARD_KEY_PAGE_DOWN;
    case SCANCODE_EXT_DELETE:
        return (char)KEYBOARD_KEY_DELETE;
    case SCANCODE_EXT_KEYPAD_ENTER:
        return '\n';
    case SCANCODE_EXT_KEYPAD_SLASH:
        return '/';
    default:
        return 0;
    }
}

/* The Windows key is the Start menu only when it is let go with nothing
   pressed in between, so that it can later be the start of a chord without
   every chord also opening the menu. */
void keyboard_super_down(keyboard_super_t *state) {
    if (!state->held) {
        state->held = 1;
        state->alone = 1;
    }
}

void keyboard_super_other_key(keyboard_super_t *state) {
    state->alone = 0;
}

int keyboard_super_up(keyboard_super_t *state) {
    int alone = state->held && state->alone;
    state->held = 0;
    state->alone = 0;
    return alone;
}
