/* user_space/libc/include/time.h - M63.
 *
 * Just enough for a program that wants to know how long it took: a
 * seconds-since-1970 clock over M59's CMOS RTC, and clock() over the
 * millisecond tick counter this OS has had since M6.
 *
 * M63 wrote here: "Deliberately no struct tm, no strftime, no
 * localtime... A program that needs them will say so at link time, which
 * is the specification." A program has now said so - CPython's
 * Python/pytime.c declares `_PyTime_localtime(time_t, struct tm *)` and
 * will not compile against a header where `struct tm` is not a real
 * type - so the specification arrived and this is the answer to it.
 *
 * The timezone objection stands and is handled by not having one:
 * `localtime` and `gmtime` are the SAME function here, because the RTC
 * is read as UTC (M59 said so) and there is nothing to convert to. A
 * program that calls localtime gets UTC and `tm_gmtoff` is 0, which is
 * true. What M63 declined to do was invent a timezone; what this does is
 * report there isn't one. */
#pragma once

#include <stddef.h>

typedef long time_t;

/* M80 groundwork: `struct timespec`, which is what every deadline API
 * written since 1993 takes - including pthread_cond_timedwait, which is
 * where this project first needed one. The clock behind it is
 * SYS_uptime_ms and SYS_time, so the nanoseconds field is real to the
 * millisecond and zero below that; saying so beats reporting a
 * nanosecond figure this machine cannot measure. */
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

/* M80 groundwork. One clock, and it is the only one this machine has:
 * CLOCK_REALTIME is SYS_time and CLOCK_MONOTONIC is SYS_uptime_ms. They
 * are named separately because a program that asks for a monotonic clock
 * is asking for one that cannot go backwards, and SYS_uptime_ms
 * genuinely cannot - SYS_time can, since SYS_settime exists. */
typedef int clockid_t;
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int clock_gettime(clockid_t clk, struct timespec *ts);
/* One millisecond, for both clocks, because that is what SYS_uptime_ms
 * reports and there is nothing finer to report. A caller that asks is
 * asking whether it can measure microseconds here; the answer is no, and
 * this is where it says so. */
int clock_getres(clockid_t clk, struct timespec *res);
/* M80 groundwork. The fields are C's, in C's order. `tm_isdst` is always
 * 0 and `tm_gmtoff` always 0 - see the header note: this machine keeps
 * UTC and knows of no other zone, so those are facts rather than
 * placeholders. */
struct tm {
    int tm_sec;    /* 0-60, 60 for a leap second */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0-6, Sunday = 0 */
    int tm_yday;   /* 0-365 */
    int tm_isdst;  /* always 0 - there is no zone here to have a rule */
    long tm_gmtoff;/* always 0 - the clock is UTC */
    const char *tm_zone; /* always "UTC" */
};

/* `localtime` and `gmtime` are the same conversion, deliberately - see
 * the header note. The _r forms are the ones to use; the others return a
 * pointer into one static struct, which is the ancient interface and is
 * provided because programs still call it. */
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);

/* The inverse. `tm_isdst` and the weekday/yearday fields are ignored on
 * input and recomputed, which is what mktime is specified to do. */
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);

/* A subset of the conversion specifiers: %Y %m %d %H %M %S %y %j %a %A
 * %b %B %p %Z %% and %F %T. Anything else is copied through literally
 * rather than silently dropped, so an unsupported specifier shows up in
 * the output where somebody will see it. */
size_t strftime(char *out, size_t max, const char *fmt, const struct tm *tm);

typedef long clock_t;

/* M6's tick is 100 Hz, so this is the honest resolution rather than the
 * conventional 1000000. */
#define CLOCKS_PER_SEC 1000

time_t time(time_t *out);
clock_t clock(void);
