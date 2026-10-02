#include "usb_hid.h"

#include "input.h"
#include "keyboard_keys.h"

/* M216: 0x4A-0x4E are Home, Page Up, Delete, End and Page Down, and
   0x54-0x58 the keypad's operators and Enter - the codes the PS/2 driver
   sends for the same keys.

   M211: usages 0x3A-0x45 are F1-F12 and 0x4F-0x52 the arrows. They were
   zero here, so a USB keyboard could not send Alt+F4 or move a selection -
   found by the input suite's first run with a USB keyboard over a test that
   closes a window that way. They are the same codes the PS/2 driver sends. */
const char usb_hid_ascii[104] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`',
    ',', '.', '/', 0,
    KEYBOARD_KEY_FUNCTION(1), KEYBOARD_KEY_FUNCTION(2), KEYBOARD_KEY_FUNCTION(3), KEYBOARD_KEY_FUNCTION(4),
    KEYBOARD_KEY_FUNCTION(5), KEYBOARD_KEY_FUNCTION(6), KEYBOARD_KEY_FUNCTION(7), KEYBOARD_KEY_FUNCTION(8),
    KEYBOARD_KEY_FUNCTION(9), KEYBOARD_KEY_FUNCTION(10), KEYBOARD_KEY_FUNCTION(11), KEYBOARD_KEY_FUNCTION(12),
    0, 0, 0, 0, KEYBOARD_KEY_HOME, KEYBOARD_KEY_PAGE_UP, KEYBOARD_KEY_DELETE, KEYBOARD_KEY_END,
    KEYBOARD_KEY_PAGE_DOWN, KEYBOARD_KEY_RIGHT, KEYBOARD_KEY_LEFT, KEYBOARD_KEY_DOWN,
    KEYBOARD_KEY_UP, 0, '/', '*', '-', '+', '\n', 0, 0, 0,
};

const char usb_hid_ascii_shift[104] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~',
    '<', '>', '?', 0,
    KEYBOARD_KEY_FUNCTION(1), KEYBOARD_KEY_FUNCTION(2), KEYBOARD_KEY_FUNCTION(3), KEYBOARD_KEY_FUNCTION(4),
    KEYBOARD_KEY_FUNCTION(5), KEYBOARD_KEY_FUNCTION(6), KEYBOARD_KEY_FUNCTION(7), KEYBOARD_KEY_FUNCTION(8),
    KEYBOARD_KEY_FUNCTION(9), KEYBOARD_KEY_FUNCTION(10), KEYBOARD_KEY_FUNCTION(11), KEYBOARD_KEY_FUNCTION(12),
    0, 0, 0, 0, KEYBOARD_KEY_HOME, KEYBOARD_KEY_PAGE_UP, KEYBOARD_KEY_DELETE, KEYBOARD_KEY_END,
    KEYBOARD_KEY_PAGE_DOWN, KEYBOARD_KEY_RIGHT, KEYBOARD_KEY_LEFT, KEYBOARD_KEY_DOWN,
    KEYBOARD_KEY_UP, 0, '/', '*', '-', '+', '\n', 0, 0, 0,
};

static int mods_from_report(uint8_t raw) {
    int mods = 0;
    if (raw & 0x22) {
        mods |= KEYBOARD_MOD_SHIFT;
    }
    if (raw & 0x11) {
        mods |= KEYBOARD_MOD_CTRL;
    }
    if (raw & 0x44) {
        mods |= KEYBOARD_MOD_ALT;
    }
    if (raw & 0x88) {
        mods |= KEYBOARD_MOD_SUPER;
    }
    return mods;
}

/* The two GUI bits of the modifier byte are the Windows keys; pressed and
   let go with nothing else, they are the Start menu - the PS/2 rule. */
#define USB_HID_GUI_BITS 0x88u

static void push_key(usb_hid_keys_t *out, char ch, int mods) {
    if (out->count < 6) {
        out->ch[out->count] = ch;
        out->mods[out->count] = mods;
        out->count++;
    }
}

void usb_hid_decode_keyboard(usb_hid_state_t *state, const uint8_t report[8],
                             usb_hid_keys_t *out) {
    out->count = 0;
    int mods = mods_from_report(report[0]);
    int gui_now = (report[0] & USB_HID_GUI_BITS) != 0;
    int gui_before = state->have_last && (state->last_keys[0] & USB_HID_GUI_BITS) != 0;
    if (gui_now && !gui_before) {
        keyboard_super_down(&state->super_key);
    }

    for (int i = 2; i < 8; i++) {
        uint8_t usage = report[i];
        if (usage <= 3 || usage >= 104) {
            continue;
        }
        int was_held = 0;
        if (state->have_last) {
            for (int j = 2; j < 8; j++) {
                if (state->last_keys[j] == usage) {
                    was_held = 1;
                    break;
                }
            }
        }
        if (was_held) {
            continue;
        }
        keyboard_super_other_key(&state->super_key);
        char ch = (mods & KEYBOARD_MOD_SHIFT) ? usb_hid_ascii_shift[usage] : usb_hid_ascii[usage];
        if (ch == 0) {
            continue;
        }
        push_key(out, ch, mods);
    }
    if (!gui_now && gui_before && keyboard_super_up(&state->super_key)) {
        push_key(out, (char)KEYBOARD_KEY_SUPER, mods);
    }

    for (int i = 0; i < 8; i++) {
        state->last_keys[i] = report[i];
    }
    state->have_last = 1;
}

void usb_hid_decode_mouse(usb_hid_state_t *state, const uint8_t *report,
                          uint8_t report_length, usb_hid_mouse_t *out) {
    out->buttons = (uint8_t)(report[0] & 0x07);
    out->dx = (int8_t)report[1];
    out->dy = (int8_t)report[2];
    out->wheel = report_length > 3 ? (int8_t)report[3] : 0;
    out->deliver = (out->dx != 0 || out->dy != 0 || out->wheel != 0 ||
                    out->buttons != state->last_buttons || !state->have_last);
    state->last_buttons = out->buttons;
    state->have_last = 1;
}
