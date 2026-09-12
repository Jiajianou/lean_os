/* kernel/drivers/xhci.h - M107
 *
 * xHCI, and enough of USB on top of it to enumerate a boot-protocol
 * keyboard and a boot-protocol mouse.
 *
 * ---- why this is in the same milestone as two disk drivers -----------
 *
 * M107's bullet puts it bluntly: "keyboard.c:12's port 0x60 has no
 * counterpart on a modern laptop, and an OS that cannot be typed at on
 * the machine in front of you is a demo whatever else is true of it."
 * Everything else in this project can be graded over a serial cable. This
 * cannot: a machine that boots to a desktop nobody can click on has not
 * booted for any purpose a person has.
 *
 * ---- what xHCI actually is, in one paragraph -------------------------
 *
 * A controller that owns an array of *device slots*, each of which has a
 * *context* the driver fills in and the controller reads, and a *ring* of
 * 16-byte TRBs per endpoint that the driver appends to and the controller
 * consumes. Every ring is circular with a Link TRB at the end, and every
 * TRB carries a cycle bit whose value flips each time the ring wraps -
 * which is how producer and consumer agree on where the new entries stop
 * without either of them writing an index the other reads. The driver
 * rings a doorbell; the controller answers on a single *event ring* that
 * carries command completions, transfer completions and port changes
 * together.
 *
 * That is genuinely more machinery than AHCI, and unlike AHCI none of it
 * is optional: there is no way to talk to a USB device without a slot, a
 * context, an address, a configuration and a transfer ring.
 *
 * ---- what this driver does not do ------------------------------------
 *
 * **Hubs.** A device plugged into a hub plugged into the machine is not
 * found. Enumerating through a hub means driving the hub as a USB device
 * of its own - its own class requests, its own port status - and every
 * machine this milestone is aimed at has its keyboard and its trackpad on
 * root ports. A USB keyboard through a dock is the case this misses, and
 * it is written down here rather than discovered later.
 *
 * **Anything that is not boot-protocol HID.** A modern keyboard speaks a
 * report descriptor this driver never reads: it asks for boot protocol,
 * which is a fixed 8-byte keyboard report and a 3-or-4-byte mouse report
 * that every HID device is required to support precisely so that a BIOS,
 * or an OS in this position, does not have to parse anything. A device
 * that refuses SET_PROTOCOL is skipped rather than guessed at.
 *
 * **Interrupts.** The event ring is drained from the PIT tick (10 ms,
 * kernel/drivers/pit.h), which is the same order as the 8 ms polling
 * interval a boot keyboard asks for, and a keystroke is delivered through
 * keyboard_inject() - the same ring buffer the PS/2 IRQ handler feeds, so
 * nothing above this can tell the two apart. That is the design decision
 * worth arguing with, and the argument for it is that it makes the USB
 * path *indistinguishable* rather than parallel: no second code path in
 * the compositor, no second kind of key event, and the entire existing
 * input suite grades it unchanged.
 *
 * ---- and one thing it does that QEMU will never exercise -------------
 *
 * `USBLEGSUP`: on a real machine the firmware owns the controller when
 * the kernel starts, because the firmware has been using it to read the
 * keyboard in its own boot menu. A driver that programs the controller
 * without asking for it first is fighting SMM code for the same
 * registers. QEMU has no such firmware and the handshake is a no-op here
 * - which is exactly why it is written now rather than found on metal.
 */
#pragma once

#include <stdint.h>

/* Finds an xHCI controller, resets it, and enumerates every HID device on
 * a root port. Returns the number of HID devices it brought up - 0 for a
 * machine with no controller, no USB input devices, or a controller that
 * would not start. */
int xhci_init(void);

/* Drains the event ring and delivers whatever arrived. Called from the
 * PIT tick, in interrupt context: it does MMIO reads and pushes into the
 * keyboard and mouse ring buffers, and does not allocate, block or take
 * any lock the scheduler holds. */
void xhci_poll(void);

/* How many HID devices came up, for the boot log and the self-test. */
int xhci_device_count(void);

/* M107: what the self-test needs to tell "the USB path delivered this"
 * from "the PS/2 driver did". Counts reports the controller has actually
 * handed over, per class. */
uint64_t xhci_keyboard_reports(void);
uint64_t xhci_mouse_reports(void);
