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
