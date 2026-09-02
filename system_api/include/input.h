/* system_api/include/input.h
 *
 * Shared ABI for input events crossing the syscall boundary (M20's
 * SYS_mouse_read). kernel/drivers/mouse.h uses this exact struct too
 * (rather than defining its own and hoping the two never drift) - one
 * definition, included on both sides, the same reasoning signal.h
 * already established for SYS_kill.
 */
#pragma once

#include <stdint.h>

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

typedef struct {
    int32_t dx; /* relative movement since the last event, screen
                 * coordinates (x: right-positive, y: down-positive -
                 * already sign-flipped from the wire protocol's
                 * up-positive Y by kernel/drivers/mouse.c) */
    int32_t dy;
    uint8_t buttons; /* bit0 = left, bit1 = right, bit2 = middle */
    /* M40: SYS_uptime_ms as of the interrupt that decoded this packet,
     * not as of whenever a reader gets around to it. Double-click
     * detection (desktop_icons.c) used to time the *processing* of two
     * clicks rather than the clicks themselves, so any delay between
     * them - a full-screen compositor redraw is easily enough - stretched
     * a genuine double-click past its own 500ms window and it silently
     * did nothing. That failed more often the more windows were open,
     * which is exactly what "double-clicking that icon doesn't work"
     * looks like from the outside. Timestamping at the source makes the
     * measurement independent of how busy everything downstream is. */
    uint32_t time_ms;
    /* M49: wheel detents since the last event - negative up, positive
     * down, matching the screen-coordinate sense dy already uses. Always
     * 0 on a 3-byte-protocol mouse, which is exactly what a device
     * without a wheel reports, so a reader never has to ask whether the
     * hardware has one. See kernel/drivers/mouse.c's IntelliMouse
     * negotiation. */
    int32_t wheel;
} mouse_event_t;

/* M32: SYS_kbd_modifiers' bitmask - which modifiers were held for the
 * character SYS_kbd_read/SYS_read most recently returned (M40; see
 * kernel/drivers/keyboard.h for why it isn't live state), for a caller
 * that needs to tell a plain keypress from a modifier chord
 * (compositor.c's Alt+Tab, gui_terminal.c's Ctrl+C/V) since the
 * decoded-ASCII stream alone can't distinguish them - 'c' is 'c' whether
 * or not Ctrl was held when it was typed. SHIFT is included for
 * completeness even though nothing currently needs it this way (the
 * decoded ASCII character is already shift-applied). */
#define KBD_MOD_CTRL  1
#define KBD_MOD_ALT   2
#define KBD_MOD_SHIFT 4

/* M33: arrow-key sentinels pushed into the same buffered-character stream
 * SYS_kbd_read/SYS_read(fd=0) already deliver (kernel/drivers/keyboard.c
 * decodes the 0xE0-prefixed extended scancodes for just these four keys -
 * still no general extended-scancode support, see that file's own header
 * comment). Values 1-4 are otherwise unreachable through this driver's
 * lookup tables (there's no Ctrl-transforms-the-character behavior here -
 * Ctrl is tracked separately, KBD_MOD_CTRL above - so this isn't the
 * classic-terminal Ctrl+A==0x01 collision it would be on a real TTY),
 * safe for user_space/bin/text_editor.c (M33) to treat as unambiguous
 * cursor-movement keys without a client thinking it typed a control
 * character. */
#define KBD_KEY_UP    1
#define KBD_KEY_DOWN  2
#define KBD_KEY_LEFT  3
#define KBD_KEY_RIGHT 4

/* M49: the function keys, in the same buffered-character stream and for
 * the same reason - Alt+F4 is one of the chords a desktop is expected to
 * have, and F4 had no representation at all in a driver that only
 * decoded printable ASCII plus the four arrows.
 *
 * 14..25 rather than continuing 5..16 from the arrows: 8, 9, 10 and 13
 * are '\b', '\t', '\n' and '\r', which this driver really does emit, and
 * 27 is Escape. 14..25 is the longest run below 32 that collides with
 * none of them, which is why the numbering has a gap in it. */
#define KBD_KEY_F1  14
#define KBD_KEY_F12 25
#define KBD_KEY_FN(n) (KBD_KEY_F1 + (n) - 1) /* n is 1..12 */

#ifdef __cplusplus
}
#endif
