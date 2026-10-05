#include "tick_clock.h"

int tick_clock_accept(const tick_clock_t *clock, uint64_t ticks, uint64_t now_tsc) {
    if (clock->cycles_per_tick == 0) {
        return 1;
    }
    uint64_t elapsed = now_tsc > clock->epoch_tsc ? now_tsc - clock->epoch_tsc : 0;
    uint64_t counted = ticks > clock->epoch_ticks ? ticks - clock->epoch_ticks : 0;
    /* The tick this interrupt would be is due at (counted + 1) periods past
       the epoch; take it from a quarter period early, which covers the
       jitter between one interrupt's latency and the epoch's. */
    return (counted + 1) * clock->cycles_per_tick <= elapsed + clock->cycles_per_tick / 4;
}

static uint32_t permille(uint64_t value, uint64_t expected) {
    if (expected == 0) {
        return 0;
    }
    uint64_t p = (value * 1000ull + expected / 2) / expected;
    return p > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)p;
}

static int within(uint32_t p) {
    return p + TIMEKEEPING_TOLERANCE_PERMILLE >= 1000u &&
           p <= 1000u + TIMEKEEPING_TOLERANCE_PERMILLE;
}

void timekeeping_grade(const timekeeping_sample_t *s, timekeeping_verdict_t *out) {
    uint64_t expected_cycles = (uint64_t)s->reference_ms * 1000ull * s->cycles_per_us;
    uint64_t expected_ticks = (uint64_t)s->reference_ms * s->tick_hz / 1000ull;
    out->tsc_permille = permille(s->tsc_cycles, expected_cycles);
    out->tick_permille = permille(s->ticks, expected_ticks);
    out->interrupt_permille = permille(s->interrupts, expected_ticks);
    out->ok = s->reference_ms > 0 && expected_ticks > 0 && expected_cycles > 0 &&
              within(out->tsc_permille) && within(out->tick_permille);
}

uint64_t tick_clock_median3(uint64_t a, uint64_t b, uint64_t c) {
    if (a > b) {
        uint64_t t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        b = c;
    }
    return a > b ? a : b;
}
