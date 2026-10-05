#include "check.h"

#include "../kernel/drivers/tick_clock.c"

/* 2.304 GHz and 100 Hz: what QEMU on the Intel Mac this was found on
   reports, so one period is 23,040,000 cycles. */
#define PERIOD 23040000ull

/* Feeds `count` interrupts, the i-th at start + i * spacing + jitter(i),
   through the rule exactly as pit_irq applies it, and returns the ticks. */
static uint64_t run(const tick_clock_t *clock, uint64_t ticks, uint64_t start, uint64_t spacing,
                    int count, const int64_t *jitter, int jitter_len) {
    for (int i = 0; i < count; i++) {
        int64_t j = jitter_len ? jitter[i % jitter_len] : 0;
        uint64_t at = start + (uint64_t)i * spacing + (uint64_t)j;
        if (tick_clock_accept(clock, ticks, at)) {
            ticks++;
        }
    }
    return ticks;
}

TEST(tick_clock, the_echo_half_a_period_after_each_edge_is_not_a_tick) {
    /* QEMU's I/O APIC: the PIT's rising edge and its re-assertion at the
       falling one, 200 interrupts a second. Ten seconds of them must be
       ten seconds of ticks - not twenty, which is what counting them gave. */
    tick_clock_t clock = {.epoch_tsc = 1000, .epoch_ticks = 50, .cycles_per_tick = PERIOD};
    uint64_t ticks = run(&clock, 50, 1000 + PERIOD, PERIOD / 2, 2000, 0, 0);
    CHECK_EQ(ticks - 50, 1000);
}

TEST(tick_clock, one_edge_a_period_loses_nothing_even_with_jitter) {
    /* The 8259, and a real I/O APIC: every interrupt is a tick, including
       one taken a fifth of a period late and one a fifth early. */
    static const int64_t jitter[] = {0, (int64_t)PERIOD / 5, 0, -(int64_t)PERIOD / 5, 1000, 0, 0};
    tick_clock_t clock = {.epoch_tsc = 5000, .epoch_ticks = 0, .cycles_per_tick = PERIOD};
    uint64_t ticks = run(&clock, 0, 5000 + PERIOD, PERIOD, 700, jitter, 7);
    CHECK_EQ(ticks, 700);
}

TEST(tick_clock, the_echoes_are_dropped_through_jitter_too) {
    /* Interrupt latency moves the echo around as well: a rule that only
       worked for an echo exactly half a period later would be one QEMU's
       timer slack defeats. Within a tenth of a period either way, still
       one tick a period. */
    static const int64_t jitter[] = {0, (int64_t)PERIOD / 10, -(int64_t)PERIOD / 10, 3000, 0};
    tick_clock_t clock = {.epoch_tsc = 0, .epoch_ticks = 0, .cycles_per_tick = PERIOD};
    uint64_t ticks = run(&clock, 0, PERIOD, PERIOD / 2, 1000, jitter, 5);
    CHECK(ticks >= 499 && ticks <= 501);
}

TEST(tick_clock, the_count_never_runs_ahead_of_the_tsc) {
    /* An interrupt storm - one every hundredth of a period - may not make
       time: after ten periods of it there are at most ten ticks. */
    tick_clock_t clock = {.epoch_tsc = 0, .epoch_ticks = 7, .cycles_per_tick = PERIOD};
    uint64_t ticks = run(&clock, 7, 1, PERIOD / 100, 1000, 0, 0);
    CHECK(ticks - 7 <= 10);
    CHECK(ticks - 7 >= 9);
}

TEST(tick_clock, a_tick_lost_to_a_stall_is_made_up_from_the_echoes) {
    /* After the host stopped the guest for five periods, interrupts that
       arrive half a period apart are all due, so the count catches up with
       the TSC rather than staying five behind forever. */
    tick_clock_t clock = {.epoch_tsc = 0, .epoch_ticks = 0, .cycles_per_tick = PERIOD};
    uint64_t ticks = run(&clock, 0, PERIOD, PERIOD / 2, 20, 0, 0);
    CHECK_EQ(ticks, 10);
    ticks = run(&clock, ticks, 16 * PERIOD, PERIOD / 2, 20, 0, 0);
    CHECK_EQ(ticks, 25);
}

TEST(tick_clock, before_calibration_every_interrupt_counts) {
    tick_clock_t clock = {0, 0, 0};
    CHECK_EQ(run(&clock, 0, 0, 1, 10, 0, 0), 10);
}

TEST(tick_clock, a_tsc_read_before_the_epoch_is_not_time) {
    /* Another core's TSC a little behind the one the epoch came from must
       not wrap into an enormous elapsed time. */
    tick_clock_t clock = {.epoch_tsc = 10 * PERIOD, .epoch_ticks = 3, .cycles_per_tick = PERIOD};
    CHECK(!tick_clock_accept(&clock, 3, 10 * PERIOD - 100));
    CHECK(tick_clock_accept(&clock, 3, 11 * PERIOD));
}

TEST(timekeeping, doubled_clocks_fail_and_true_ones_pass) {
    /* The two boots that found this, as [timekeeping] measured them. */
    timekeeping_sample_t broken = {.reference_ms = 2000, .tsc_cycles = 4608000000ull,
                                   .cycles_per_us = 1152, .ticks = 400, .interrupts = 400,
                                   .tick_hz = 100};
    timekeeping_verdict_t v;
    timekeeping_grade(&broken, &v);
    CHECK(!v.ok);
    CHECK_EQ(v.tsc_permille, 2000);
    CHECK_EQ(v.tick_permille, 2000);

    timekeeping_sample_t fixed = {.reference_ms = 2000, .tsc_cycles = 4608000000ull,
                                  .cycles_per_us = 2304, .ticks = 200, .interrupts = 400,
                                  .tick_hz = 100};
    timekeeping_grade(&fixed, &v);
    CHECK(v.ok);
    CHECK_EQ(v.tsc_permille, 1000);
    CHECK_EQ(v.tick_permille, 1000);
    CHECK_EQ(v.interrupt_permille, 2000);
}

TEST(timekeeping, each_clock_is_graded_on_its_own) {
    timekeeping_verdict_t v;
    /* The TSC right and the tick doubled. */
    timekeeping_sample_t tick = {2000, 4608000000ull, 2304, 400, 400, 100};
    timekeeping_grade(&tick, &v);
    CHECK(!v.ok);
    /* The tick right and the TSC calibrated at half. */
    timekeeping_sample_t tsc = {2000, 4608000000ull, 1152, 200, 200, 100};
    timekeeping_grade(&tsc, &v);
    CHECK(!v.ok);
    /* Both at the edges of the tolerance, either side. */
    timekeeping_sample_t edge = {2000, 4608000000ull * 11 / 10, 2304, 180, 180, 100};
    timekeeping_grade(&edge, &v);
    CHECK(v.ok);
    timekeeping_sample_t past = {2000, 4608000000ull, 2304, 179, 179, 100};
    timekeeping_grade(&past, &v);
    CHECK(!v.ok);
    /* No reference at all is not a pass. */
    timekeeping_sample_t none = {0, 0, 2304, 0, 0, 100};
    timekeeping_grade(&none, &v);
    CHECK(!v.ok);
}

TEST(tick_clock, the_median_of_three_is_the_middle_one) {
    CHECK_EQ(tick_clock_median3(1, 2, 3), 2);
    CHECK_EQ(tick_clock_median3(3, 1, 2), 2);
    CHECK_EQ(tick_clock_median3(2, 3, 1), 2);
    CHECK_EQ(tick_clock_median3(3, 2, 1), 2);
    CHECK_EQ(tick_clock_median3(5, 5, 1), 5);
    CHECK_EQ(tick_clock_median3(9, 4, 9), 9);
}
