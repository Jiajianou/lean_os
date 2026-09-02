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

/* ---- M89: sleeping, in the three spellings a program uses ------------
 *
 * All three are one mechanism: SYS_waitfds with nothing to wait for and
 * a deadline, which is the wait this system already has - see poll.c,
 * which found the same thing one milestone earlier and said so.
 *
 * The resolution is a millisecond, because that is what SYS_waitfds
 * takes and what clock_getres above already reports. A nanosleep of 100
 * microseconds therefore sleeps for zero and returns 0 rather than
 * sleeping a whole millisecond: rounding *up* would make a program that
 * polls in a tight loop 10x slower than it asked for, and rounding down
 * to zero is what a machine with a millisecond clock can honestly do.
 * Sub-millisecond remainders round up to 1 ms only when the whole
 * request is below 1 ms and non-zero, so `nanosleep(1ns)` still yields.
 *
 * `rem` is written with zeros on a completed sleep. This machine's wait
 * is not interrupted by a signal handler returning - see M76 - so a
 * short sleep is not a case that arises, and reporting a remainder of
 * zero is the truth rather than a simplification.
 */
int nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
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

/* M89: the timezone globals and the call that sets them.
 *
 * This machine keeps UTC and knows of no other zone - the header note
 * above says so and `localtime` is `gmtime` for that reason. So tzset is
 * a call that sets `tzname` to {"UTC", "UTC"}, `timezone` to 0 and
 * `daylight` to 0, every time, and a program that calls it and then
 * reads those gets consistent answers rather than uninitialized ones.
 * TZ in the environment is deliberately ignored: honouring it would need
 * a zone database, and pretending to honour it would put a program an
 * hour off with no way to tell. */
extern char *tzname[2];
extern long timezone;
extern int daylight;
void tzset(void);

/* M89: strftime's inverse. Supports the conversions a program actually
 * parses - %Y %y %m %d %e %H %I %M %S %j %b %B %a %A %p %T %D %F %R %n
 * %t %% and %s - and returns a pointer to the first character it did not
 * consume, or NULL if the format did not match. An unsupported
 * conversion returns NULL rather than being skipped, for the reason
 * scanf.c gives about the same choice. */
char *strptime(const char *s, const char *format, struct tm *tm);


/* The inverse. `tm_isdst` and the weekday/yearday fields are ignored on
 * input and recomputed, which is what mktime is specified to do. */
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);

/* A subset of the conversion specifiers: %Y %m %d %H %M %S %y %j %a %A
 * %b %B %p %Z %% and %F %T. Anything else is copied through literally
 * rather than silently dropped, so an unsupported specifier shows up in
 * the output where somebody will see it. */
size_t strftime(char *out, size_t max, const char *fmt, const struct tm *tm);

/* M89: the 1970s spelling - "Www Mmm dd hh:mm:ss yyyy\n", 26 bytes
 * including the newline and the NUL, always. It is here because a
 * program that prints a timestamp without formatting one calls it, and
 * because `dmesg -T` does.
 *
 * `ctime` and `asctime` return a pointer into one static buffer, which
 * is the ancient interface; the _r forms take the caller's 26 bytes and
 * are the ones to use. All four produce the same string - there is one
 * timezone here, so ctime and a UTC asctime cannot differ. */
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *t);
char *ctime_r(const time_t *t, char *buf);

typedef long clock_t;

/* M6's tick is 100 Hz, so this is the honest resolution rather than the
 * conventional 1000000. */
#define CLOCKS_PER_SEC 1000

time_t time(time_t *out);
clock_t clock(void);
