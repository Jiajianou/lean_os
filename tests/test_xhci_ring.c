#include "check.h"

#include "drivers/xhci_ring.h"

#include <stdlib.h>
#include <string.h>

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

static int consumer_take(consumer_t *c, xhci_trb_t *out) {
    for (int hops = 0; hops < 4; hops++) {
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

TEST(xhci_ring, everything_pushed_comes_back_in_order_across_many_laps) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    consumer_t c;
    consumer_init(&c, buf);

    for (int i = 0; i < 1000; i++) {
        xhci_ring_push(&r, PARAM_OF(i), 8, (1u << XHCI_TRB_TYPE_SHIFT));
        xhci_trb_t got;
        REQUIRE(consumer_take(&c, &got) == 1);
        CHECK_EQ(got.param, PARAM_OF(i));
        CHECK_EQ(got.status, 8);
    }
    free(buf);
}

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
    xhci_trb_t got;
    CHECK_EQ(consumer_take(&c, &got), 0);
    free(buf);
}

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

TEST(xhci_ring, a_lap_one_entry_short_of_full_still_drains) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);
    consumer_t c;
    consumer_init(&c, buf);

    const int per_lap = XHCI_RING_TRBS - 2;
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

    xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    CHECK(buf[0].control & XHCI_TRB_CYCLE);

    for (int i = 1; i < XHCI_RING_TRBS - 1; i++) {
        xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    }
    xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    CHECK_EQ(buf[0].control & XHCI_TRB_CYCLE, 0u);
    free(buf);
}

TEST(xhci_ring, the_link_cycle_follows_the_lap_that_reached_it) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    for (int i = 0; i < XHCI_RING_TRBS - 1; i++) {
        xhci_ring_push(&r, 0, 0, (1u << XHCI_TRB_TYPE_SHIFT));
    }
    CHECK(buf[XHCI_RING_TRBS - 1].control & XHCI_TRB_CYCLE);
    CHECK_EQ(r.cycle, 0);

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
        uint32_t type = (buf[XHCI_RING_TRBS - 1].control >> XHCI_TRB_TYPE_SHIFT) & 0x3F;
        CHECK_EQ(type, XHCI_TRB_LINK);
        CHECK_EQ(buf[XHCI_RING_TRBS - 1].param, 0x40000);
    }
    free(buf);
}

TEST(xhci_ring, reset_clears_every_entry_it_was_given) {
    xhci_trb_t *buf = malloc(XHCI_RING_TRBS * sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    memset(buf, 0xAB, XHCI_RING_TRBS * sizeof(xhci_trb_t));

    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);

    for (int i = 0; i < XHCI_RING_TRBS - 1; i++) {
        CHECK_EQ(buf[i].param, 0ULL);
        CHECK_EQ(buf[i].status, 0u);
        CHECK_EQ(buf[i].control, 0u);
    }
    CHECK_EQ(buf[XHCI_RING_TRBS - 1].status, 0u);
    CHECK_EQ(buf[XHCI_RING_TRBS - 1].param, 0x40000ULL);

    consumer_t c;
    consumer_init(&c, buf);
    xhci_trb_t got;
    CHECK_EQ(consumer_take(&c, &got), 0);
    free(buf);
}

TEST(xhci_ring, reset_writes_nothing_past_the_ring) {
    xhci_trb_t *buf = malloc(XHCI_RING_TRBS * sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);
    CHECK_EQ(r.index, 0u);
    CHECK_EQ(r.cycle, 1);
    free(buf);
}

TEST(xhci_ring, a_second_reset_starts_the_cycle_over) {
    xhci_trb_t *buf = calloc(XHCI_RING_TRBS, sizeof(xhci_trb_t));
    REQUIRE(buf != NULL);
    xhci_ring_t r;
    xhci_ring_reset(&r, buf, 0x40000);
    for (int i = 0; i < XHCI_RING_TRBS + 3; i++) {
        xhci_ring_push(&r, PARAM_OF(i), 0, (1u << XHCI_TRB_TYPE_SHIFT));
    }
    CHECK_EQ(r.cycle, 0);

    xhci_ring_reset(&r, buf, 0x40000);
    CHECK_EQ(r.cycle, 1);
    CHECK_EQ(r.index, 0u);
    consumer_t c;
    consumer_init(&c, buf);
    xhci_trb_t got;
    CHECK_EQ(consumer_take(&c, &got), 0);
    free(buf);
}
