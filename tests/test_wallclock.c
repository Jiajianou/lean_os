/* tests/test_wallclock.c - M116
 *
 * gettimeofday's arithmetic, against a clock that knows the true time.
 *
 * Each test drives a simulated machine: a true time in milliseconds, an
 * RTC that reports its floor in whole seconds, and an uptime counter that
 * started at some arbitrary phase relative to the RTC's second - which is
 * the whole of what went wrong before M116, so every test runs at every
 * phase from 0 to 999 ms.
 *
 * What is asserted is what a program is entitled to assume about the time
 * of day, and what NetSurf's scheduler did assume: it never goes
 * backwards, it never runs ahead of the truth, it is less than a second
 * behind it, and once it has seen the RTC's second change it is exactly
 * as good as the millisecond clock under it. The pre-M116 formula - RTC
 * seconds plus uptime modulo 1000 - fails the first of those at 999 of
 * the 1000 phases, which is checked here too so the property cannot pass
 * by being too weak to notice. */
#include "check.h"

#include "../user_space/libc/src/wallclock.h"

#include <stdint.h>

#define EPOCH_MS 1789000000000LL /* a date in 2026, in ms */

/* The formula gettimeofday used from M80 to M116. */
static long long old_formula(long long rtc_s, long long up_ms) {
    return rtc_s * 1000 + up_ms % 1000;
}

TEST(wallclock, monotonic_accurate_and_locked_at_every_phase) {
    int old_went_backwards = 0;
    for (long long phase = 0; phase < 1000; phase++) {
        wallclock_t w = {0, 0};
        long long boot = EPOCH_MS + phase; /* true time at uptime 0 */
        long long prev = 0, old_prev = 0;
        int seen_edge = 0;
        long long first_rtc = boot / 1000;
        for (long long up = 5000; up < 8000; up++) { /* three seconds, every ms */
            long long truth = boot + up;
            long long rtc = truth / 1000;
            long long t = wallclock_ms(&w, rtc, up);

            if (up > 5000 && t < prev) {
                CHECK(t >= prev);
                return; /* one report per phase is plenty */
            }
            CHECK(t <= truth);
            CHECK(truth - t < 1000);
            if (rtc != first_rtc + 5) {
                seen_edge = 1;
            }
            if (seen_edge) {
                /* Called every millisecond, so after an edge it is exact
                 * to within the one millisecond between calls. */
                CHECK(truth - t <= 1);
            }
            prev = t;

            long long o = old_formula(rtc, up);
            if (up > 5000 && o < old_prev) {
                old_went_backwards = 1;
            }
            old_prev = o;
        }
    }
    CHECK(old_went_backwards);
}

/* Between two RTC edges the time of day advances exactly as the
 * millisecond clock does - which is the property a scheduler's "in 10
 * ms" needs, and the one the old formula could break by 990 ms. */
TEST(wallclock, a_ten_millisecond_timer_fires_after_ten_milliseconds) {
    for (long long phase = 0; phase < 1000; phase += 7) {
        wallclock_t w = {0, 0};
        long long boot = EPOCH_MS + phase;
        long long up = 20000;
        (void)wallclock_ms(&w, (boot + up) / 1000, up);
        for (int n = 0; n < 500; n++, up += 3) {
            long long now = wallclock_ms(&w, (boot + up) / 1000, up);
            long long due = now + 10;
            /* The scheduler's check, 10 ms of uptime later. */
            long long later = wallclock_ms(&w, (boot + up + 10) / 1000, up + 10);
            CHECK(later >= due);
            CHECK(later - due < 1000); /* late only by an edge catch-up, never a second */
        }
    }
}

/* SYS_settime - which is what nettime does with an SNTP answer - can
 * move the RTC either way. Forward is an ordinary catch-up; backwards is
 * the one step a CLOCK_REALTIME is allowed to take, and it is taken. */
TEST(wallclock, the_clock_being_set_is_followed_both_ways) {
    wallclock_t w = {0, 0};
    long long boot = EPOCH_MS + 321;
    long long up = 10000;
    long long t0 = wallclock_ms(&w, (boot + up) / 1000, up);

    long long offset = 3600 * 1000; /* set forward an hour */
    up += 50;
    long long t1 = wallclock_ms(&w, (boot + offset + up) / 1000, up);
    CHECK(t1 - t0 >= offset);
    CHECK(t1 <= boot + offset + up);

    offset = -60 * 1000; /* then back a minute */
    up += 50;
    long long t2 = wallclock_ms(&w, (boot + offset + up) / 1000, up);
    CHECK(t2 < t1);
    CHECK(t2 <= boot + offset + up);
    CHECK(boot + offset + up - t2 < 1000);
}

/* A PIT that loses ticks under load - which TCG does - makes uptime run
 * slow. The time of day must still never go backwards and must be pulled
 * forward at every RTC edge rather than falling further and further
 * behind. */
TEST(wallclock, a_slow_millisecond_clock_is_pulled_forward_not_left_behind) {
    wallclock_t w = {0, 0};
    long long boot = EPOCH_MS + 555;
    long long prev = 0;
    for (long long truth_up = 0; truth_up < 20000; truth_up++) {
        long long up = truth_up * 9 / 10; /* 10% of ticks lost */
        long long truth = boot + truth_up;
        long long t = wallclock_ms(&w, truth / 1000, up);
        CHECK(t >= prev);
        CHECK(t <= truth);
        CHECK(truth - t < 1000);
        prev = t;
    }
}
