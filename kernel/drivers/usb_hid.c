#include "usb_hid.h"

#include "input.h" /* system_api/include/input.h - KBD_MOD_* */

/* HID usage codes, as the boot protocol numbers them: 0x04 is 'a',
 * 0x1E is '1', 0x28 is Return. The table is indexed by usage code
 * directly, so index 0-3 are the four "no key / error" codes and are
 * deliberately zero.
 *
 * F1-F12 (0x3A-0x45) and the navigation block (0x46-0x52) are zero, and
 * that is the same position keyboard.h takes for the PS/2 driver's
 * 0xE0-prefixed scancodes: this kernel's input path carries ASCII plus a
 * modifier mask, so a key with no character has nothing to travel as.
 * Inventing a code for them here would deliver bytes no reader in this
 * tree knows how to interpret, which is worse than delivering nothing. */
const char usb_hid_ascii[104] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`',
    ',', '.', '/', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

const char usb_hid_ascii_shift[104] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~',
    '<', '>', '?', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* Modifier byte: bit 0 left Ctrl, 1 left Shift, 2 left Alt, 3 left GUI,
 * and bits 4-7 the same four on the right. Both sides fold to one bit
 * here because system_api/include/input.h has one bit for each, and
 * because nothing in this tree has ever distinguished them - the PS/2
 * driver decodes left Ctrl and left Alt only. */
static int mods_from_report(uint8_t raw) {
    int mods = 0;
    if (raw & 0x22) {
        mods |= KBD_MOD_SHIFT;
    }
    if (raw & 0x11) {
        mods |= KBD_MOD_CTRL;
    }
    if (raw & 0x44) {
        mods |= KBD_MOD_ALT;
    }
    return mods;
}

void usb_hid_decode_keyboard(usb_hid_state_t *state, const uint8_t report[8],
                             usb_hid_keys_t *out) {
    out->count = 0;
    int mods = mods_from_report(report[0]);

    /* Byte 1 is reserved, and a value of 0x01 in the key slots is
     * ErrorRollOver - what a keyboard sends when more keys are held than
     * it can report. Neither is a key. */
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
        char ch = (mods & KBD_MOD_SHIFT) ? usb_hid_ascii_shift[usage] : usb_hid_ascii[usage];
        if (ch == 0) {
            continue;
        }
        if (out->count < 6) {
            out->ch[out->count] = ch;
            out->mods[out->count] = mods;
            out->count++;
        }
    }

    for (int i = 0; i < 8; i++) {
        state->last_keys[i] = report[i];
    }
    state->have_last = 1;
}

void usb_hid_decode_mouse(usb_hid_state_t *state, const uint8_t *report,
                          uint8_t report_len, usb_hid_mouse_t *out) {
    out->buttons = (uint8_t)(report[0] & 0x07);
    out->dx = (int8_t)report[1];
    out->dy = (int8_t)report[2];
    out->wheel = report_len > 3 ? (int8_t)report[3] : 0;
    out->deliver = (out->dx != 0 || out->dy != 0 || out->wheel != 0 ||
                    out->buttons != state->last_buttons || !state->have_last);
    state->last_buttons = out->buttons;
    state->have_last = 1;
}
