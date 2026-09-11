#include "wallclock.h"

/* How far this clock may run ahead of the RTC's second before that is
 * taken as the clock having been set backwards rather than as the two
 * counters drifting. A second is the most the phase can ever account
 * for; the quarter-second on top is for a PIT that runs slightly fast
 * against the RTC, so drift is never mistaken for a step. */
#define WALLCLOCK_STEP_BACK_MS 1250

long long wallclock_ms(wallclock_t *w, long long rtc_seconds, long long uptime_ms) {
    long long rtc_ms = rtc_seconds * 1000;
    if (!w->valid) {
        w->base_ms = rtc_ms - uptime_ms;
        w->valid = 1;
    }
    long long now = w->base_ms + uptime_ms;
    if (now < rtc_ms) {
        /* The RTC has entered a second this clock has not reached: move
         * up to its edge. Forward only, so time never runs backwards. */
        w->base_ms = rtc_ms - uptime_ms;
        now = rtc_ms;
    } else if (now >= rtc_ms + WALLCLOCK_STEP_BACK_MS) {
        /* The RTC is behind by more than phase and drift can explain:
         * somebody set the clock back. Follow it. */
        w->base_ms = rtc_ms - uptime_ms;
        now = rtc_ms;
    }
    return now;
}
