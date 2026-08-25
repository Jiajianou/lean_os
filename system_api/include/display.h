/* system_api/include/display.h
 *
 * M58: the display-mode ABI - what SYS_display_modes hands back and what
 * SYS_display_set_mode takes. Shared by the kernel's mode-setting driver
 * (kernel/drivers/dispi.h) and by settings.c's Display pane, so there is
 * one definition rather than two that can drift.
 */
#pragma once

#include <stdint.h>

/* Deliberately small. This is a curated list of standard sizes, not an
 * enumeration - the DISPI interface has no mode table to enumerate (it
 * accepts any geometry inside its own limits), so "the modes" are a
 * judgement the driver makes, and a judgement that runs to more than a
 * dozen entries is a list nobody can read. */
#define DISPLAY_MAX_MODES 12

typedef struct {
    uint32_t width;
    uint32_t height;
} display_mode_t;
