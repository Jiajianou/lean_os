#pragma once

typedef struct {
    long long base_ns;
    int valid;
} wallclock_t;

long long wallclock_ns(wallclock_t *w, long long rtc_seconds, long long uptime_ns);
long long wallclock_ms(wallclock_t *w, long long rtc_seconds, long long uptime_ms);
