/* kernel/drivers/usb_hid.h - M107
 *
 * A USB boot-protocol HID report, decoded without the hardware.
 *
 * Split out of xhci.c for the reason rtl8139_ring.h gives for its own
 * existence, and it is the same category of risk: everything else in that
 * driver either works or fails loudly - a controller that is programmed
 * wrongly does not enumerate, and that is visible in one line of the boot
 * log. This is the part that can be *plausibly wrong*. A keyboard whose
 * reports are decoded slightly incorrectly still types; it types a
 * doubled letter when a key is held, or drops the second of two keys
 * pressed together, or reports Shift as Ctrl. None of those stop a boot
 * and none of them are visible in a serial log.
 *
 * ---- what a boot report is -------------------------------------------
 *
 * The USB HID specification defines a fixed report layout that every
 * keyboard and mouse must support, precisely so that a BIOS - or an OS in
 * this position - can read one without parsing a report descriptor.
 *
 * A keyboard's is eight bytes: a modifier bitmap, a reserved byte, and
 * six usage codes for the keys currently held. It is STATE, not events:
 * holding a key means the same byte appears in every report until it is
 * released, and a keystroke is therefore a difference between two
 * reports rather than a report. Getting that wrong is what produces a
 * keyboard that repeats every held key a hundred times a second, which
 * is the bug this file exists to make testable.
 *
 * A mouse's is three or four: a button bitmap, then signed X, Y and -
 * on a device that has one - a wheel delta. Those ARE events, and the
 * only subtlety is that a report with no movement and no button change
 * is one the device is entitled to send and this must not forward.
 */
#pragma once

#include <stdint.h>

/* The state one device carries between reports. Zeroed for a device that
 * has just been enumerated, which is correct: no keys held, no buttons
 * down. */
typedef struct {
    uint8_t last_keys[8];
    uint8_t last_buttons;
    int have_last;
} usb_hid_state_t;

/* What a keyboard report decoded into: the new presses, as ASCII, with
 * the modifier mask each was pressed with.
 *
 * Six is the ceiling because a boot report holds six usage codes, so six
 * new presses in one report is the most that can ever be reported - which
 * is a fact about the protocol rather than a chosen limit. */
typedef struct {
    char ch[6];
    int mods[6];
    int count;
} usb_hid_keys_t;

/* Decodes an 8-byte keyboard report against the previous one, filling
 * *out with the keys that are newly down. Updates *state.
 *
 * A key held across two reports produces nothing the second time. A usage
 * code this kernel has no character for - a function key, the navigation
 * block - produces nothing at all, exactly as the PS/2 driver produces
 * nothing for the extended scancodes it does not decode (keyboard.h says
 * so in its own header). Both are silence rather than a wrong key. */
void usb_hid_decode_keyboard(usb_hid_state_t *state, const uint8_t report[8],
                             usb_hid_keys_t *out);

/* What a mouse report decoded into. `deliver` is 0 for a report that says
 * nothing changed, which the caller must not forward - a mouse polled
 * every 10 ms sends a great many of those, and each one forwarded would
 * wake the compositor to move the cursor by zero. */
typedef struct {
    int32_t dx, dy, wheel;
    uint8_t buttons;
    int deliver;
} usb_hid_mouse_t;

void usb_hid_decode_mouse(usb_hid_state_t *state, const uint8_t *report,
                          uint8_t report_len, usb_hid_mouse_t *out);

/* The two translation tables, exposed for the test that grades them
 * against each other: a shifted table whose unshifted twin has a hole in
 * it, or vice versa, is a key that types one character and not the
 * other. */
extern const char usb_hid_ascii[104];
extern const char usb_hid_ascii_shift[104];
