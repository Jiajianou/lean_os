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

/* M51: pushes a synthetic event onto the same ring the IRQ handler feeds,
 * timestamped the same way, so a reader (SYS_mouse_read, and therefore
 * the compositor) cannot tell it from a real packet. It exists for the
 * boot self-tests: the [m51] z-order test has to deliver a real click to
 * a real hit-test, and until now the kernel had no way to move a pointer
 * at all - every mouse-driven behavior this project has was reachable
 * only from tools/qemu-input-test.sh, which a boot-time test cannot run.
 *
 * Deliberately not a syscall. A user program that could forge input would
 * be able to click any other program's window, and nothing in user space
 * has ever needed to; this is a kernel-internal test hook, which is why
 * it takes the decoded event rather than a PS/2 packet - the framing
 * mouse_irq does is not what any of this is trying to exercise. */
void mouse_inject(int32_t dx, int32_t dy, uint8_t buttons, int32_t wheel);
