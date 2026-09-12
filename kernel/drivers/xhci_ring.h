/* kernel/drivers/xhci_ring.h - M107
 *
 * An xHCI transfer/command ring, without the controller.
 *
 * The second of the two pieces of drivers/xhci.c that can be plausibly
 * wrong rather than loudly wrong (drivers/usb_hid.h is the first). The
 * cycle bit is the whole synchronisation protocol between this driver and
 * the controller, and it is four lines of arithmetic:
 *
 *   - every TRB carries a cycle bit, and the controller consumes entries
 *     for exactly as long as that bit matches its own idea of the current
 *     cycle;
 *   - the last entry in the ring is a Link TRB pointing back at the
 *     start, with Toggle Cycle set;
 *   - when the producer reaches the Link, it writes the Link's cycle bit
 *     to the value it has been using, and then flips.
 *
 * Get the flip one entry early and the controller stops at the Link
 * forever - a keyboard that works for fifteen keystrokes and then goes
 * silent, on a machine with no other symptom. Get the Link's own cycle
 * bit wrong and it stops on the first lap. Neither is visible in a boot
 * log, both need a ring that has actually wrapped, and a boot test would
 * have to type fifteen characters to see it. A host test does it in a
 * loop.
 *
 * ---- the one rule a caller must keep ---------------------------------
 *
 * The producer must never advance past the consumer - the ordinary
 * ring-full rule, with a sharper edge than usual here. A consumer that
 * has taken the last entry of a lap is sitting ON the Link and follows it
 * only on its next step; a producer that completes another whole lap in
 * that window rewrites the Link's cycle bit, and the consumer arrives at
 * a Link that no longer matches and stops for good.
 *
 * xhci.c keeps the rule with room to spare: a control transfer is three
 * TRBs and an interrupt endpoint has one outstanding, against fifteen
 * usable entries. This is not enforced here because enforcing it would
 * mean this unit tracking a dequeue pointer the controller owns - the
 * driver would have to read it back from the endpoint context, which is
 * real work for a bound nothing in this driver approaches. It is written
 * down instead, and tests/test_xhci_ring.c asserts the boundary from
 * both sides so the next caller finds it stated rather than discovers it.
 */
#pragma once

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

/* 15 usable entries plus the Link. Small on purpose: a control transfer
 * is three TRBs and an interrupt endpoint has one outstanding, so the
 * ring's depth is never the limit - but a small ring wraps often, which
 * means the cycle-bit logic above is exercised constantly in normal use
 * rather than once an hour. */
#define XHCI_RING_TRBS 16

#define XHCI_TRB_CYCLE      (1u << 0)
#define XHCI_TRB_TC         (1u << 1)
#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_LINK       6

typedef struct {
    xhci_trb_t *trb;
    uint64_t phys;
    uint32_t index;
    uint8_t cycle;
} xhci_ring_t;

/* Lays out a ring in `buf` (XHCI_RING_TRBS entries, zeroed by this) whose
 * physical address is `phys`, and writes the Link TRB. */
void xhci_ring_reset(xhci_ring_t *r, void *buf, uint64_t phys);

/* Appends one TRB and advances, handling the Link and the cycle flip.
 * The caller passes `control` WITHOUT a cycle bit; this adds it, and
 * writes it last of the four dwords - the controller may be reading the
 * ring concurrently and the cycle bit is what tells it the other three
 * are there. */
void xhci_ring_push(xhci_ring_t *r, uint64_t param, uint32_t status, uint32_t control);
