/* user_space/libc/src/wallclock.h - M116
 *
 * The time of day, to the millisecond, from two clocks that were never
 * meant to be added together.
 *
 * This machine has a wall clock that counts whole seconds (the RTC,
 * through SYS_time) and a monotonic clock that counts milliseconds since
 * boot (the PIT, through SYS_uptime_ms). Until M116 gettimeofday() was
 * the first plus the second modulo 1000 - and those two counters are not
 * in phase, so the result jumped backwards by up to a second whenever
 * the uptime's millisecond wrapped before the RTC's second did. Its own
 * comment said so.
 *
 * What that cost is why this file exists. NetSurf's scheduler sets every
 * timer as "now plus N ms" by gettimeofday and runs it when gettimeofday
 * passes it; the fetch poller reschedules itself every 10 ms. A clock
 * that steps back 900 ms turns "in 10 ms" into "in 910 ms", with the CPU
 * idle and the reply sitting in the socket - google.com took 22 seconds
 * to load, and a packet capture showed a 301 redirect arriving at 0.14 s
 * and being read at 4.03 s.
 *
 * The rule: a process's time of day is its uptime plus one offset, and
 * the offset only ever moves forward - to the moment the RTC is seen to
 * have entered a second this clock has not reached yet. So it is
 * monotonic, its milliseconds are the monotonic clock's own, and it
 * trails the true time by less than a second at worst and by about one
 * call interval once it has watched a second change. The one exception
 * is the clock being SET backwards (SYS_settime, from nettime), which
 * POSIX lets CLOCK_REALTIME do and which is recognised by the RTC being
 * more than a second behind. */
#pragma once

typedef struct {
    long long base_ms; /* wall-clock ms at uptime 0; meaningful once valid */
    int valid;
} wallclock_t;

/* The time of day in ms since 1970, given the RTC's current whole
 * seconds and the monotonic uptime in ms. Updates `w`. */
long long wallclock_ms(wallclock_t *w, long long rtc_seconds, long long uptime_ms);
