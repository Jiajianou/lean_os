#pragma once

typedef struct {
    long long base_ms;
    int valid;
} wallclock_t;

long long wallclock_ms(wallclock_t *w, long long rtc_seconds, long long uptime_ms);
