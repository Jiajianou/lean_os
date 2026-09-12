/* tests/test_xhci_ring.c - M107
 *
 * The cycle bit, which is the entire handshake between this driver and an
 * xHCI controller, wrapped a thousand times in a millisecond.
 *
 * The failure this catches: a producer that flips its cycle bit one entry
 * early leaves a Link TRB whose cycle no longer matches, and the
 * controller - which follows the ring for exactly as long as the bits
 * agree - stops there and never restarts. On the machine that is a
 * keyboard which works for fifteen keystrokes and then goes silent, with
 * nothing in the boot log and no other symptom. Fifteen, because the ring
 * has fifteen usable entries.
 *
 * So the consumer below is written the way the specification says a
 * controller behaves, and then run over the producer for several laps.
 * That is the same method tests/test_rtl8139_ring.c uses and for the same
 * reason: the test must model the OTHER side, or it only proves the
 * driver agrees with itself.
 */
#include "check.h"

#include "drivers/xhci_ring.h"

#include <stdlib.h>
#include <string.h>

/* ---- the controller, as the specification describes it ---------------
 *
 * It holds a dequeue pointer and a cycle-state bit. It consumes an entry
 * when that entry's cycle bit equals its cycle state, and stops when it
 * does not. A Link TRB is followed rather than consumed, and its Toggle
 * Cycle bit flips the controller's cycle state as it goes. */
typedef struct {
    const xhci_trb_t *ring;
    uint32_t index;
    uint8_t cycle;
} consumer_t;

static void consumer_init(consumer_t *c, const xhci_trb_t *ring) {
    c->ring = ring;
    c->index = 0;
    c->cycle = 1;
}

/* Consumes one TRB into *out and returns 1, or returns 0 when the
 * controller would stop. Link TRBs are followed transparently. */
static int consumer_take(consumer_t *c, xhci_trb_t *out) {
    for (int hops = 0; hops < 4; hops++) { /* at most one Link between entries */
        const xhci_trb_t *t = &c->ring[c->index];
        if ((t->control & XHCI_TRB_CYCLE) != c->cycle) {
            return 0;
        }
        uint32_t type = (t->control >> XHCI_TRB_TYPE_SHIFT) & 0x3F;
        if (type == XHCI_TRB_LINK) {
            if (t->control & XHCI_TRB_TC) {
                c->cycle ^= 1;
            }
            c->index = 0;
            continue;
        }
        *out = *t;
        c->index++;
        return 1;
    }
    return 0;
}

#define PARAM_OF(i) (0x1000ULL + (uint64_t)(i))

TEST(xhci_ring, a_fresh_ring_offers_nothing) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    consumer_t c;
    consumer_init(&c, buf);
    xhci_trb_t got;
    /* The Link is present and its cycle bit matches, so a consumer
     * follows it straight back to entry 0 - which is empty. It must stop
     * there rather than loop. */
    CHECK_EQ(consumer_take(&c, &got), 0);
    free(buf);
}

TEST(xhci_ring, the_link_trb_points_back_at_the_start) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    const xhci_trb_t *link = &buf[XHCI_RING_TRBS - 1];
    CHECK_EQ(link->param, 0x40000);
    CHECK_EQ((link->control >> XHCI_TRB_TYPE_SHIFT) & 0x3F, XHCI_TRB_LINK);
    CHECK(link->control & XHCI_TRB_TC);
    free(buf);
}

/* The test this file exists for: everything pushed comes back, in order,
 * across many laps. A cycle bit flipped one entry early fails on lap two. */
TEST(xhci_ring, everything_pushed_comes_back_in_order_across_many_laps) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    consumer_t c;
    consumer_init(&c, buf);

    /* Push one and take one, a thousand times. That is exactly what an
     * interrupt endpoint does - one transfer outstanding, re-queued on
     * each completion - so a thousand rounds is about ten seconds of a
     * real keyboard. */
    for (int i = 0; i < 1000; i++) {
        xhci_ring_push(&r, PARAM_OF(i), 8, (1u << XHCI_TRB_TYPE_SHIFT));
        xhci_trb_t got;
        REQUIRE(consumer_take(&c, &got) == 1);
        CHECK_EQ(got.param, PARAM_OF(i));
        CHECK_EQ(got.status, 8);
    }
    free(buf);
}

/* Filling the ring before draining it is what a control transfer does -
 * three TRBs go on before the doorbell rings. */
TEST(xhci_ring, a_full_lap_pushed_before_any_is_taken) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    const int usable = XHCI_RING_TRBS - 1;
    for (int i = 0; i < usable; i++) {
        xhci_ring_push(&r, PARAM_OF(i), 0, (1u << XHCI_TRB_TYPE_SHIFT));
    }

    consumer_t c;
    consumer_init(&c, buf);
    for (int i = 0; i < usable; i++) {
        xhci_trb_t got;
        REQUIRE(consumer_take(&c, &got) == 1);
        CHECK_EQ(got.param, PARAM_OF(i));
    }
    /* And then it stops: the next entry is the one the producer will
     * write on its second lap, and it still carries the old cycle. */
    xhci_trb_t got;
    CHECK_EQ(consumer_take(&c, &got), 0);
    free(buf);
}

/* ---- the ring-full rule, which this test found -----------------------
 *
 * The first version of this test pushed a whole lap (15 entries), then
 * consumed a whole lap, three times over, and it FAILED on the second
 * lap. That is not a bug in xhci_ring.c; it is the ring-full rule of
 * every circular buffer, and the interesting part is the shape it takes
 * here.
 *
 * A consumer that has just taken the last entry of a lap is sitting ON
 * the Link TRB and has not yet followed it - it follows it on its next
 * call. If the producer completes another whole lap in that window, it
 * rewrites the Link's cycle bit to its own new value, and the consumer,
 * still on the old cycle, arrives at a Link that no longer matches and
 * stops permanently.
 *
 * So the producer must never advance past the consumer, which is the
 * ordinary rule and is why xhci.c is safe: it has at most three TRBs
 * outstanding (a control transfer's Setup, Data and Status) against
 * fifteen usable entries, and an interrupt endpoint has one. This test
 * is the realistic pattern - a transfer queued and completed, over and
 * over - and it is the one that would catch a cycle flip in the wrong
 * place.
 */
TEST(xhci_ring, three_trbs_at_a_time_across_many_laps) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);
    consumer_t c;
    consumer_init(&c, buf);

    int taken = 0;
    for (int round = 0; round < 300; round++) {
        for (int i = 0; i < 3; i++) {
            xhci_ring_push(&r, PARAM_OF(round * 3 + i), 0, (1u << XHCI_TRB_TYPE_SHIFT));
        }
        for (int i = 0; i < 3; i++) {
            xhci_trb_t got;
            REQUIRE(consumer_take(&c, &got) == 1);
            CHECK_EQ(got.param, PARAM_OF(taken));
            taken++;
        }
    }
    CHECK_EQ(taken, 900);
    free(buf);
}

/* And the boundary itself, asserted rather than left implicit: a lap that
 * is one entry short of full leaves the consumer able to follow the Link,
 * which is what makes the rule above a rule and not a coincidence. */
TEST(xhci_ring, a_lap_one_entry_short_of_full_still_drains) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);
    consumer_t c;
    consumer_init(&c, buf);

    const int per_lap = XHCI_RING_TRBS - 2; /* one short of the usable count */
    int taken = 0;
    for (int lap = 0; lap < 4; lap++) {
        for (int i = 0; i < per_lap; i++) {
            xhci_ring_push(&r, PARAM_OF(lap * per_lap + i), 0, (1u << XHCI_TRB_TYPE_SHIFT));
        }
        for (int i = 0; i < per_lap; i++) {
            xhci_trb_t got;
            REQUIRE(consumer_take(&c, &got) == 1);
            CHECK_EQ(got.param, PARAM_OF(taken));
            taken++;
        }
    }
    CHECK_EQ(taken, 4 * per_lap);
    free(buf);
}

TEST(xhci_ring, the_cycle_bit_is_added_rather_than_taken_from_the_caller) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    /* The caller passes control bits with no cycle bit in them. On the
     * first lap the ring adds a 1; on the second it adds a 0. A caller
     * that had to know which would get it wrong somewhere. */
    xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    CHECK(buf[0].control & XHCI_TRB_CYCLE);

    for (int i = 1; i < XHCI_RING_TRBS - 1; i++) {
        xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    }
    /* Second lap: entry 0 again, and now the cycle bit must be 0. */
    xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    CHECK_EQ(buf[0].control & XHCI_TRB_CYCLE, 0u);
    free(buf);
}

/* The Link TRB's cycle bit must be rewritten to the cycle the producer
 * was using when it reached the Link - not the one it flips to. */
TEST(xhci_ring, the_link_cycle_follows_the_lap_that_reached_it) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    for (int i = 0; i < XHCI_RING_TRBS - 1; i++) {
        xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    }
    /* Lap one was cycle 1, so the Link must still read 1 - the consumer
     * is about to arrive at it still on cycle 1. */
    CHECK(buf[XHCI_RING_TRBS - 1].control & XHCI_TRB_CYCLE);
    CHECK_EQ(r.cycle, 0); /* ...and the producer has moved on */

    for (int i = 0; i < XHCI_RING_TRBS - 1; i++) {
        xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    }
    CHECK_EQ(buf[XHCI_RING_TRBS - 1].control & XHCI_TRB_CYCLE, 0u);
    CHECK_EQ(r.cycle, 1);
    free(buf);
}

TEST(xhci_ring, the_producer_never_writes_the_link_entry_as_data) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    for (int i = 0; i < 500; i++) {
        xhci_ring_push(&r, PARAM_OF(i), 99, (1u << XHCI_TRB_TYPE_SHIFT));
        /* The last entry must stay a Link, forever. A producer that
         * wrote data there would hand the controller a ring with no way
         * back to the start. */
        uint32_t type = (buf[XHCI_RING_TRBS - 1].control >> XHCI_TRB_TYPE_SHIFT) & 0x3F;
        CHECK_EQ(type, XHCI_TRB_LINK);
        CHECK_EQ(buf[XHCI_RING_TRBS - 1].param, 0x40000);
    }
    free(buf);
}

/* ---- What the mutation harness found ---------------------------------
 *
 * `make mutate` named the reset path: the zeroing loop and the Link
 * TRB's own fields survived every test above, because every one of them
 * pushes something before it looks. A ring that is not fully cleared
 * hands the controller whatever the page held before - and this driver's
 * pages come from pmm_alloc_contiguous, which does not zero. */
TEST(xhci_ring, reset_clears_every_entry_it_was_given) {
    xhci_trb_t *buf = malloc(XHCI_RING_TRBS * sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    /* Dirty, the way a recycled physical page is dirty. */
    memset(buf, 0xAB, XHCI_RING_TRBS * sizeof(xhci_trb_t));

    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    /* Every entry but the Link must be entirely zero - param, status and
     * control. A leftover cycle bit in any of them is a TRB the
     * controller will execute. */
    for (int i = 0; i < XHCI_RING_TRBS - 1; i++) {
        CHECK_EQ(buf[i].param, 0ULL);
        CHECK_EQ(buf[i].status, 0u);
        CHECK_EQ(buf[i].control, 0u);
    }
    /* And the Link's status must be zero too: a Link TRB's status field
     * is reserved, and a controller is entitled to object to a reserved
     * field that is not zero. */
    CHECK_EQ(buf[XHCI_RING_TRBS - 1].status, 0u);
    CHECK_EQ(buf[XHCI_RING_TRBS - 1].param, 0x40000ULL);

    /* A consumer starting on a freshly reset ring must find nothing -
     * which is the property all of that adds up to. */
    consumer_t c;
    consumer_init(&c, buf);
    xhci_trb_t got;
    CHECK_EQ(consumer_take(&c, &got), 0);
    free(buf);
}

/* Kills: the `i < XHCI_RING_TRBS` -> `i <= XHCI_RING_TRBS` mutant on the
 * zeroing loop, which writes one TRB past the buffer. Malloc'd at
 * exactly the ring size so ASan sees it. */
TEST(xhci_ring, reset_writes_nothing_past_the_ring) {
    xhci_trb_t *buf = malloc(XHCI_RING_TRBS * sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);
    CHECK_EQ(r.index, 0u);
    CHECK_EQ(r.cycle, 1);
    free(buf);
}

/* A ring reset twice - which is what re-enumerating a device after a
 * failure would do - must come back to exactly its initial state rather
 * than to whatever the previous laps left. */
TEST(xhci_ring, a_second_reset_starts_the_cycle_over) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);
    for (int i = 0; i < XHCI_RING_TRBS + 3; i++) {
        xhci_ring_push(&r, PARAM_OF(i), 0, (1u << XHCI_TRB_TYPE_SHIFT));
    }
    CHECK_EQ(r.cycle, 0); /* it has wrapped once */

    xhci_ring_reset(&r, buf, 0x40000);
    CHECK_EQ(r.cycle, 1);
    CHECK_EQ(r.index, 0u);
    consumer_t c;
    consumer_init(&c, buf);
    xhci_trb_t got;
    CHECK_EQ(consumer_take(&c, &got), 0);
    free(buf);
}
