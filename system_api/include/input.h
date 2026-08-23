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

typedef struct {
    int32_t dx; /* relative movement since the last event, screen
                 * coordinates (x: right-positive, y: down-positive -
                 * already sign-flipped from the wire protocol's
                 * up-positive Y by kernel/drivers/mouse.c) */
    int32_t dy;
    uint8_t buttons; /* bit0 = left, bit1 = right, bit2 = middle */
} mouse_event_t;

/* M32: SYS_kbd_modifiers' bitmask - kernel/drivers/keyboard.h's live
 * (not buffered) held/not-held state, for a caller that needs to tell a
 * plain keypress from a modifier chord (compositor.c's Alt+Tab,
 * gui_terminal.c's Ctrl+C/V) since the decoded-ASCII stream
 * SYS_kbd_read/SYS_read already deliver can't distinguish them - 'c' is
 * 'c' whether or not Ctrl was held when it was typed. SHIFT is included
 * for completeness even though nothing currently needs it this way (the
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
