/* kernel/drivers/mouse.h
 *
 * PS/2 mouse driver, IRQ12 (the auxiliary device line off the same 8042
 * controller keyboard.c already talks to). Standard 3-byte packet
 * protocol, same shape of design as keyboard.c: an IRQ handler decodes
 * into a small ring buffer, mouse_read() drains it non-blocking.
 */
#pragma once

#include <stdint.h>

#include "input.h" /* system_api/include/input.h - mouse_event_t, shared with
                     * user space (M20's SYS_mouse_read) so there's exactly
                     * one definition instead of two that could drift apart */

void mouse_init(void);

/* Pops the oldest buffered event into *ev and returns 1, or returns 0
 * (leaving *ev untouched) if none is available. Never blocks. */
int mouse_read(mouse_event_t *ev);
