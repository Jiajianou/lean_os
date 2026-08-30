#include <time.h>

#include "syscall_wrappers.h"

time_t time(time_t *out) {
    long now = sys_time((os_datetime_t *)0);
    if (out) {
        *out = (time_t)now;
    }
    return (time_t)now;
}

clock_t clock(void) {
    /* Milliseconds since boot, which is what CLOCKS_PER_SEC above says
     * this returns. Not CPU time: this OS does not account per-task CPU
     * time at all, and reporting wall time as if it were would be a
     * quieter lie than reporting wall time and saying so. */
    return (clock_t)sys_uptime_ms();
}

/* M80 groundwork - see <sys/time.h> for what the microseconds field can
 * and cannot say. */
#include <sys/time.h>

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) {
        return -1;
    }
    tv->tv_sec = (time_t)sys_time((os_datetime_t *)0);
    /* The sub-second part comes from the uptime clock, which is the only
     * thing here that ticks faster than a second. It is not phase-locked
     * to the wall clock - the two are different counters - so this is
     * "some number of milliseconds within the current second" rather
     * than "the fraction of this second that has elapsed". Enough for a
     * program measuring intervals, and not enough for one setting a
     * clock; the second one should use SYS_uptime_ms directly. */
    tv->tv_usec = (long)(sys_uptime_ms() % 1000) * 1000;
    return 0;
}

int clock_gettime(clockid_t clk, struct timespec *ts) {
    if (!ts) {
        return -1;
    }
    if (clk == CLOCK_MONOTONIC) {
        /* SYS_uptime_ms, which genuinely cannot go backwards - that is
         * the whole property a caller asks for by naming this clock. */
        long ms = sys_uptime_ms();
        ts->tv_sec = ms / 1000;
        ts->tv_nsec = (ms % 1000) * 1000000L;
        return 0;
    }
    if (clk == CLOCK_REALTIME) {
        ts->tv_sec = (time_t)sys_time((os_datetime_t *)0);
        ts->tv_nsec = (long)(sys_uptime_ms() % 1000) * 1000000L; /* see gettimeofday */
        return 0;
    }
    return -1;
}

int clock_getres(clockid_t clk, struct timespec *res) {
    if (!res || (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC)) {
        return -1;
    }
    /* One millisecond. drivers/pit.h ticks at 100 Hz and SYS_uptime_ms
     * reports milliseconds; there is nothing finer to report and a
     * smaller number here would be a claim this machine cannot keep. */
    res->tv_sec = 0;
    res->tv_nsec = 1000000L;
    return 0;
}

/* ---- M80 groundwork: struct tm ---------------------------------------
 *
 * The calendar arithmetic is os_time.h's, not a second copy: M59 already
 * had to turn a Unix time into a civil date to draw a file's mtime, and
 * `os_civil_from_unix`/`os_days_from_civil` are shared by the kernel and
 * user space precisely so that "what day is this" has one answer here.
 */
static void tm_from_unix(time_t t, struct tm *out) {
    os_datetime_t d;
    os_civil_from_unix((uint32_t)t, &d);
    out->tm_sec = d.second;
    out->tm_min = d.minute;
    out->tm_hour = d.hour;
    out->tm_mday = d.day;
    out->tm_mon = d.month - 1;
    out->tm_year = (int)d.year - 1900;
    /* 1970-01-01 was a Thursday, which is where the +4 comes from. */
    out->tm_wday = (int)(((t / 86400) + 4) % 7);
    if (out->tm_wday < 0) {
        out->tm_wday += 7;
    }
    out->tm_yday = (int)(os_days_from_civil(d.year, d.month, d.day) -
                          os_days_from_civil(d.year, 1, 1));
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = "UTC";
}

struct tm *gmtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) {
        return 0;
    }
    tm_from_unix(*t, out);
    return out;
}

/* The same conversion. See <time.h>: this machine keeps UTC and knows of
 * no other zone, so a localtime that differed would be inventing one. */
struct tm *localtime_r(const time_t *t, struct tm *out) {
    return gmtime_r(t, out);
}

static struct tm tm_static;

struct tm *gmtime(const time_t *t) {
    return gmtime_r(t, &tm_static);
}

struct tm *localtime(const time_t *t) {
    return gmtime_r(t, &tm_static);
}

time_t timegm(struct tm *tm) {
    if (!tm) {
        return (time_t)-1;
    }
    /* Normalised through the same shared helper, then read back so that
     * tm_wday and tm_yday come out right - which is what mktime is
     * specified to do and what a caller relies on. */
    uint32_t secs = os_days_from_civil((uint16_t)(tm->tm_year + 1900),
                                        (uint8_t)(tm->tm_mon + 1),
                                        (uint8_t)tm->tm_mday) * 86400u +
                    (uint32_t)tm->tm_hour * 3600u +
                    (uint32_t)tm->tm_min * 60u + (uint32_t)tm->tm_sec;
    tm_from_unix((time_t)secs, tm);
    return (time_t)secs;
}

time_t mktime(struct tm *tm) {
    return timegm(tm); /* one zone, and it is UTC */
}

static const char *const WDAY_SHORT[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const WDAY_LONG[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                          "Thursday", "Friday", "Saturday"};
static const char *const MON_SHORT[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const MON_LONG[12] = {"January", "February", "March", "April", "May",
                                          "June", "July", "August", "September", "October",
                                          "November", "December"};

static size_t put_str(char *out, size_t max, size_t at, const char *s) {
    while (*s && at + 1 < max) {
        out[at++] = *s++;
    }
    return at;
}

static size_t put_num(char *out, size_t max, size_t at, int v, int width) {
    char buf[16];
    int n = 0;
    int neg = v < 0;
    unsigned uv = (unsigned)(neg ? -v : v);
    if (uv == 0) {
        buf[n++] = '0';
    }
    while (uv > 0) {
        buf[n++] = (char)('0' + uv % 10);
        uv /= 10;
    }
    while (n < width) {
        buf[n++] = '0';
    }
    if (neg) {
        buf[n++] = '-';
    }
    while (n > 0 && at + 1 < max) {
        out[at++] = buf[--n];
    }
    return at;
}

size_t strftime(char *out, size_t max, const char *fmt, const struct tm *tm) {
    if (!out || !fmt || !tm || max == 0) {
        return 0;
    }
    size_t at = 0;
    for (const char *p = fmt; *p && at + 1 < max; p++) {
        if (*p != '%') {
            out[at++] = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 'Y': at = put_num(out, max, at, tm->tm_year + 1900, 1); break;
        case 'y': at = put_num(out, max, at, (tm->tm_year + 1900) % 100, 2); break;
        case 'm': at = put_num(out, max, at, tm->tm_mon + 1, 2); break;
        case 'd': at = put_num(out, max, at, tm->tm_mday, 2); break;
        case 'e': at = put_num(out, max, at, tm->tm_mday, 1); break;
        case 'H': at = put_num(out, max, at, tm->tm_hour, 2); break;
        case 'M': at = put_num(out, max, at, tm->tm_min, 2); break;
        case 'S': at = put_num(out, max, at, tm->tm_sec, 2); break;
        case 'j': at = put_num(out, max, at, tm->tm_yday + 1, 3); break;
        case 'a': at = put_str(out, max, at, WDAY_SHORT[tm->tm_wday % 7]); break;
        case 'A': at = put_str(out, max, at, WDAY_LONG[tm->tm_wday % 7]); break;
        case 'b': case 'h': at = put_str(out, max, at, MON_SHORT[tm->tm_mon % 12]); break;
        case 'B': at = put_str(out, max, at, MON_LONG[tm->tm_mon % 12]); break;
        case 'p': at = put_str(out, max, at, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'Z': at = put_str(out, max, at, "UTC"); break;
        case 'F':
            at = put_num(out, max, at, tm->tm_year + 1900, 4);
            at = put_str(out, max, at, "-");
            at = put_num(out, max, at, tm->tm_mon + 1, 2);
            at = put_str(out, max, at, "-");
            at = put_num(out, max, at, tm->tm_mday, 2);
            break;
        case 'T':
            at = put_num(out, max, at, tm->tm_hour, 2);
            at = put_str(out, max, at, ":");
            at = put_num(out, max, at, tm->tm_min, 2);
            at = put_str(out, max, at, ":");
            at = put_num(out, max, at, tm->tm_sec, 2);
            break;
        case '%': out[at++] = '%'; break;
        case '\0': p--; break;
        default:
            /* Copied through rather than dropped - an unsupported
             * specifier that vanishes is a bug nobody sees, and one that
             * appears as "%q" in the output is one somebody reports. */
            if (at + 2 < max) {
                out[at++] = '%';
                out[at++] = *p;
            }
            break;
        }
    }
    out[at] = '\0';
    return at;
}
