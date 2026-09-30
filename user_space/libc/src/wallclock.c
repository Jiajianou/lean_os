#include "wallclock.h"

#define WALLCLOCK_STEP_BACK_NS 1250000000LL

long long wallclock_ns(wallclock_t *w, long long rtc_seconds, long long uptime_ns) {
    long long rtc_ns = rtc_seconds * 1000000000LL;
    if (!w->valid) {
        w->base_ns = rtc_ns - uptime_ns;
        w->valid = 1;
    }
    long long now = w->base_ns + uptime_ns;
    if (now < rtc_ns) {
        w->base_ns = rtc_ns - uptime_ns;
        now = rtc_ns;
    } else if (now >= rtc_ns + WALLCLOCK_STEP_BACK_NS) {
        w->base_ns = rtc_ns - uptime_ns;
        now = rtc_ns;
    }
    return now;
}

long long wallclock_ms(wallclock_t *w, long long rtc_seconds, long long uptime_ms) {
    return wallclock_ns(w, rtc_seconds, uptime_ms * 1000000LL) / 1000000LL;
}
