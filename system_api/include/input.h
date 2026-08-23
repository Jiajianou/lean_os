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
