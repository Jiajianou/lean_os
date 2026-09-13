#include "check.h"

#include "../user_space/libc/src/wallclock.h"

#include <stdint.h>

#define EPOCH_MS 1789000000000LL

static long long old_formula(long long rtc_s, long long up_ms) {
    return rtc_s * 1000 + up_ms % 1000;
}

TEST(wallclock, monotonic_accurate_and_locked_at_every_phase) {
    int old_went_backwards = 0;
    for (long long phase = 0; phase < 1000; phase++) {
        wallclock_t w = {0, 0};
        long long boot = EPOCH_MS + phase;
        long long prev = 0, old_previous = 0;
        int seen_edge = 0;
        long long first_rtc = boot / 1000;
        for (long long up = 5000; up < 8000; up++) {
            long long truth = boot + up;
            long long rtc = truth / 1000;
            long long t = wallclock_ms(&w, rtc, up);

            if (up > 5000 && t < prev) {
                CHECK(t >= prev);
                return;
            }
            CHECK(t <= truth);
            CHECK(truth - t < 1000);
            if (rtc != first_rtc + 5) {
                seen_edge = 1;
            }
            if (seen_edge) {
                CHECK(truth - t <= 1);
            }
            prev = t;

            long long o = old_formula(rtc, up);
            if (up > 5000 && o < old_previous) {
                old_went_backwards = 1;
            }
            old_previous = o;
        }
    }
    CHECK(old_went_backwards);
}

TEST(wallclock, a_ten_millisecond_timer_fires_after_ten_milliseconds) {
    for (long long phase = 0; phase < 1000; phase += 7) {
        wallclock_t w = {0, 0};
        long long boot = EPOCH_MS + phase;
        long long up = 20000;
        (void)wallclock_ms(&w, (boot + up) / 1000, up);
        for (int n = 0; n < 500; n++, up += 3) {
            long long now = wallclock_ms(&w, (boot + up) / 1000, up);
            long long due = now + 10;
            long long later = wallclock_ms(&w, (boot + up + 10) / 1000, up + 10);
            CHECK(later >= due);
            CHECK(later - due < 1000);
        }
    }
}

TEST(wallclock, the_clock_being_set_is_followed_both_ways) {
    wallclock_t w = {0, 0};
    long long boot = EPOCH_MS + 321;
    long long up = 10000;
    long long t0 = wallclock_ms(&w, (boot + up) / 1000, up);

    long long offset = 3600 * 1000;
    up += 50;
    long long t1 = wallclock_ms(&w, (boot + offset + up) / 1000, up);
    CHECK(t1 - t0 >= offset);
    CHECK(t1 <= boot + offset + up);

    offset = -60 * 1000;
    up += 50;
    long long t2 = wallclock_ms(&w, (boot + offset + up) / 1000, up);
    CHECK(t2 < t1);
    CHECK(t2 <= boot + offset + up);
    CHECK(boot + offset + up - t2 < 1000);
}

TEST(wallclock, a_slow_millisecond_clock_is_pulled_forward_not_left_behind) {
    wallclock_t w = {0, 0};
    long long boot = EPOCH_MS + 555;
    long long prev = 0;
    for (long long truth_up = 0; truth_up < 20000; truth_up++) {
        long long up = truth_up * 9 / 10;
        long long truth = boot + truth_up;
        long long t = wallclock_ms(&w, truth / 1000, up);
        CHECK(t >= prev);
        CHECK(t <= truth);
        CHECK(truth - t < 1000);
        prev = t;
    }
}
