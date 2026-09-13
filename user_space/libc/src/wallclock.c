#include "wallclock.h"

#define WALLCLOCK_STEP_BACK_MS 1250

long long wallclock_ms(wallclock_t *w, long long rtc_seconds, long long uptime_ms) {
    long long rtc_ms = rtc_seconds * 1000;
    if (!w->valid) {
        w->base_ms = rtc_ms - uptime_ms;
        w->valid = 1;
    }
    long long now = w->base_ms + uptime_ms;
    if (now < rtc_ms) {
        w->base_ms = rtc_ms - uptime_ms;
        now = rtc_ms;
    } else if (now >= rtc_ms + WALLCLOCK_STEP_BACK_MS) {
        w->base_ms = rtc_ms - uptime_ms;
        now = rtc_ms;
    }
    return now;
}
