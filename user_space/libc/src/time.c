#include <time.h>
#include <errno.h>
#include <stdint.h>   /* M89: nanosleep refuses an out-of-range timespec */
#include <unistd.h> /* M88: sysconf(_SC_CLK_TCK), the rate clock() scales from */

#include "proc.h" /* system_api/include/proc.h - os_rusage_t, M88 */
#include "syscall_wrappers.h"

time_t time(time_t *out) {
    long now = sys_time((os_datetime_t *)0);
    if (out) {
        *out = (time_t)now;
    }
    return (time_t)now;
}

clock_t clock(void) {
    /* M88: processor time, which is what clock() actually means.
     *
     * Until this milestone this returned uptime with a comment saying so
     * - "this OS does not account per-task CPU time at all, and reporting
     * wall time as if it were would be a quieter lie than reporting wall
     * time and saying so." That was the right call while it was true. It
     * stopped being true when SYS_rusage landed, so this is the real
     * thing now: user plus system ticks, scaled to CLOCKS_PER_SEC.
     *
     * The scaling is exact rather than approximate - the tick is 10 ms
     * and CLOCKS_PER_SEC is 1000, so one tick is 10 clocks and no
     * rounding is involved. If the two ever stop dividing evenly, this
     * expression still holds; it just stops being exact. */
    os_rusage_t r;
    if (sys_rusage(OS_RUSAGE_SELF, &r) != 0) {
        return (clock_t)-1;
    }
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        return (clock_t)-1;
    }
    return (clock_t)((r.user_ticks + r.sys_ticks) * (unsigned long)CLOCKS_PER_SEC / (unsigned long)hz);
}

/* M80 groundwork - see <sys/time.h> for what the microseconds field can
 * and cannot say. */
#include <sys/time.h>

#include "wallclock.h"

/* M116: the time of day, and it no longer runs backwards.
 *
 * This was RTC seconds plus uptime-modulo-1000 for the fraction - two
 * counters not in phase, so a pair of calls either side of the uptime's
 * millisecond wrapping could go back by most of a second. M98 found that
 * once (/bin/measure reported an assembler finishing 530 ms before it
 * started) and answered it by telling interval-measurers to use
 * CLOCK_MONOTONIC. That was right for this project's own programs and no
 * help at all to somebody else's: NetSurf's scheduler times everything
 * with gettimeofday, and a 10 ms timer that meets a 900 ms step back
 * fires 910 ms late. See wallclock.h for what that did to page loads,
 * and for the rule that replaced it. One clock per process. */
static wallclock_t realtime_clock;

static long long realtime_ms(void) {
    return wallclock_ms(&realtime_clock, (long long)sys_time((os_datetime_t *)0),
                        (long long)sys_uptime_ms());
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) {
        return -1;
    }
    long long ms = realtime_ms();
    tv->tv_sec = (time_t)(ms / 1000);
    tv->tv_usec = (long)(ms % 1000) * 1000;
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
        long long ms = realtime_ms(); /* M116: see gettimeofday */
        ts->tv_sec = (time_t)(ms / 1000);
        ts->tv_nsec = (long)(ms % 1000) * 1000000L;
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

/* ---- M89: sleeping - see <time.h> for the resolution argument ------- */

static void sleep_ms(long ms) {
    if (ms <= 0) {
        return;
    }
    int dummy = -1;
    (void)sys_waitfds(&dummy, 0, (int)ms);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L ||
        req->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    long ms = req->tv_sec * 1000L + req->tv_nsec / 1000000L;
    if (ms == 0 && (req->tv_sec != 0 || req->tv_nsec != 0)) {
        ms = 1; /* a non-zero request never becomes a no-op */
    }
    sleep_ms(ms);
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    sleep_ms((long)seconds * 1000L);
    return 0; /* nothing cuts a sleep short here - see <time.h> */
}

int usleep(unsigned int usec) {
    long ms = (long)(usec / 1000u);
    if (ms == 0 && usec != 0) {
        ms = 1;
    }
    sleep_ms(ms);
    return 0;
}

/* ---- M89: the timezone globals - see <time.h> ------------------------ */

static char tz_utc[] = "UTC";
char *tzname[2] = {tz_utc, tz_utc};
long timezone = 0;
int daylight = 0;

void tzset(void) {
    /* Already what they are, and set again anyway so that a program
     * which zeroes them and calls this gets them back. TZ is ignored -
     * see <time.h> for why honouring it halfway would be worse than not
     * at all. */
    tzname[0] = tz_utc;
    tzname[1] = tz_utc;
    timezone = 0;
    daylight = 0;
}

/* ---- M89: strptime ---------------------------------------------------
 *
 * The mirror of strftime, sharing its month and day name tables so the
 * two cannot disagree about spelling - which is the bug a second table
 * would eventually produce.
 */
static const char *const MON_FULL[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
static const char *const DAY_FULL[7] = {"Sunday",   "Monday", "Tuesday",
                                        "Wednesday", "Thursday", "Friday",
                                        "Saturday"};

static int ci_prefix(const char *s, const char *word, int abbrev_len) {
    /* Matches `word` case-insensitively, either in full or at exactly
     * `abbrev_len` characters. Returns how many characters matched, or
     * 0. Full first, so that "June" is not read as "Jun" plus a stray
     * 'e'. */
    int i = 0;
    while (word[i]) {
        char a = s[i];
        char b = word[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            break;
        }
        i++;
    }
    if (!word[i]) {
        return i;
    }
    return i >= abbrev_len ? abbrev_len : 0;
}

static const char *scan_num(const char *s, int width, int *out) {
    int v = 0;
    int n = 0;
    while (*s == ' ') {
        s++; /* %e and a space-padded %d */
    }
    while (n < width && *s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
        n++;
    }
    if (n == 0) {
        return 0;
    }
    *out = v;
    return s;
}

char *strptime(const char *s, const char *format, struct tm *tm) {
    if (!s || !format || !tm) {
        return (char *)0;
    }
    int pm_seen = 0;
    int twelve_hour = 0;

    for (const char *f = format; *f; f++) {
        if (*f == ' ' || *f == '\t' || *f == '\n') {
            while (*s == ' ' || *s == '\t' || *s == '\n') {
                s++;
            }
            continue;
        }
        if (*f != '%') {
            if (*s != *f) {
                return (char *)0;
            }
            s++;
            continue;
        }
        f++;
        int v = 0;
        switch (*f) {
        case '%':
            if (*s++ != '%') {
                return (char *)0;
            }
            break;
        case 'n':
        case 't':
            while (*s == ' ' || *s == '\t' || *s == '\n') {
                s++;
            }
            break;
        case 'Y':
            if (!(s = scan_num(s, 4, &v))) {
                return (char *)0;
            }
            tm->tm_year = v - 1900;
            break;
        case 'y':
            if (!(s = scan_num(s, 2, &v))) {
                return (char *)0;
            }
            /* POSIX's window: 69-99 is 1900s, 00-68 is 2000s. */
            tm->tm_year = v >= 69 ? v : v + 100;
            break;
        case 'm':
            if (!(s = scan_num(s, 2, &v)) || v < 1 || v > 12) {
                return (char *)0;
            }
            tm->tm_mon = v - 1;
            break;
        case 'd':
        case 'e':
            if (!(s = scan_num(s, 2, &v)) || v < 1 || v > 31) {
                return (char *)0;
            }
            tm->tm_mday = v;
            break;
        case 'H':
            if (!(s = scan_num(s, 2, &v)) || v > 23) {
                return (char *)0;
            }
            tm->tm_hour = v;
            break;
        case 'I':
            if (!(s = scan_num(s, 2, &v)) || v < 1 || v > 12) {
                return (char *)0;
            }
            tm->tm_hour = v % 12;
            twelve_hour = 1;
            break;
        case 'M':
            if (!(s = scan_num(s, 2, &v)) || v > 59) {
                return (char *)0;
            }
            tm->tm_min = v;
            break;
        case 'S':
            if (!(s = scan_num(s, 2, &v)) || v > 60) {
                return (char *)0;
            }
            tm->tm_sec = v;
            break;
        case 'j':
            if (!(s = scan_num(s, 3, &v)) || v < 1 || v > 366) {
                return (char *)0;
            }
            tm->tm_yday = v - 1;
            break;
        case 'b':
        case 'B':
        case 'h': {
            int i;
            for (i = 0; i < 12; i++) {
                int n = ci_prefix(s, MON_FULL[i], 3);
                if (n) {
                    s += n;
                    tm->tm_mon = i;
                    break;
                }
            }
            if (i == 12) {
                return (char *)0;
            }
            break;
        }
        case 'a':
        case 'A': {
            int i;
            for (i = 0; i < 7; i++) {
                int n = ci_prefix(s, DAY_FULL[i], 3);
                if (n) {
                    s += n;
                    tm->tm_wday = i;
                    break;
                }
            }
            if (i == 7) {
                return (char *)0;
            }
            break;
        }
        case 'p': {
            int n = ci_prefix(s, "PM", 2);
            if (n) {
                pm_seen = 1;
                s += n;
            } else if ((n = ci_prefix(s, "AM", 2)) != 0) {
                s += n;
            } else {
                return (char *)0;
            }
            break;
        }
        case 's': {
            /* Seconds since the epoch, which is not a field of struct tm
             * but a whole time - so it fills every field. */
            long secs = 0;
            int n = 0;
            int neg = 0;
            if (*s == '-') {
                neg = 1;
                s++;
            }
            while (*s >= '0' && *s <= '9') {
                secs = secs * 10 + (*s - '0');
                s++;
                n++;
            }
            if (n == 0) {
                return (char *)0;
            }
            time_t t = neg ? -secs : secs;
            gmtime_r(&t, tm);
            break;
        }
        /* The composite conversions, expanded rather than duplicated:
         * each one is its own expansion parsed by this same loop. */
        case 'T':
        case 'D':
        case 'F':
        case 'R': {
            const char *sub = (*f == 'T')   ? "%H:%M:%S"
                              : (*f == 'D') ? "%m/%d/%y"
                              : (*f == 'F') ? "%Y-%m-%d"
                                            : "%H:%M";
            char *end = strptime(s, sub, tm);
            if (!end) {
                return (char *)0;
            }
            s = end;
            break;
        }
        default:
            return (char *)0; /* unsupported - see <time.h> */
        }
    }
    if (twelve_hour && pm_seen) {
        tm->tm_hour += 12;
    }
    return (char *)s;
}

/* ---- M89: asctime/ctime - see <time.h> ------------------------------
 *
 * The format is fixed and so is the length: exactly 26 bytes with the
 * newline and the NUL, which is what makes the _r forms' "give me a
 * 26-byte buffer" contract safe. Written with explicit padding rather
 * than through snprintf so that a year outside 1000-9999 cannot silently
 * change the field widths, which is the one way this string stops being
 * 26 bytes.
 */
static const char *const WDAY_ABBR[7] = {"Sun", "Mon", "Tue", "Wed",
                                         "Thu", "Fri", "Sat"};
static const char *const MON_ABBR[12] = {"Jan", "Feb", "Mar", "Apr",
                                         "May", "Jun", "Jul", "Aug",
                                         "Sep", "Oct", "Nov", "Dec"};

static void two(char *out, int v, char pad) {
    out[0] = (v / 10) ? (char)('0' + (v / 10) % 10) : pad;
    out[1] = (char)('0' + v % 10);
}

char *asctime_r(const struct tm *tm, char *buf) {
    if (!tm || !buf) {
        return (char *)0;
    }
    int wday = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    int mon = (tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0;
    int year = tm->tm_year + 1900;
    if (year < 0) {
        year = 0;
    }
    for (int i = 0; i < 3; i++) {
        buf[i] = WDAY_ABBR[wday][i];
        buf[4 + i] = MON_ABBR[mon][i];
    }
    buf[3] = ' ';
    buf[7] = ' ';
    two(buf + 8, tm->tm_mday, ' ');
    buf[10] = ' ';
    two(buf + 11, tm->tm_hour, '0');
    buf[13] = ':';
    two(buf + 14, tm->tm_min, '0');
    buf[16] = ':';
    two(buf + 17, tm->tm_sec, '0');
    buf[19] = ' ';
    buf[20] = (char)('0' + (year / 1000) % 10);
    buf[21] = (char)('0' + (year / 100) % 10);
    buf[22] = (char)('0' + (year / 10) % 10);
    buf[23] = (char)('0' + year % 10);
    buf[24] = '\n';
    buf[25] = '\0';
    return buf;
}

static char asctime_buf[26];

char *asctime(const struct tm *tm) {
    return asctime_r(tm, asctime_buf);
}

char *ctime_r(const time_t *t, char *buf) {
    struct tm tm;
    if (!t || !gmtime_r(t, &tm)) {
        return (char *)0;
    }
    return asctime_r(&tm, buf);
}

char *ctime(const time_t *t) {
    return ctime_r(t, asctime_buf);
}

int settimeofday(const struct timeval *tv, const void *tz) {
    (void)tz; /* POSIX says to ignore it, and there is one zone anyway */
    if (!tv) {
        errno = EFAULT;
        return -1;
    }
    if (tv->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    if (sys_settime((uint32_t)tv->tv_sec) != 0) {
        /* The syscall refuses an implausible time - see SYS_settime. */
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* M97: seconds between two times, as a double.
 *
 * A time_t here is a signed count of seconds, so this is a subtraction -
 * but the signature is the portable one because a program that calls it
 * was written against the portable one, where time_t need not be
 * arithmetic at all. The cast is done on the difference rather than on
 * each operand so that the exact integer result is what gets converted. */
double difftime(time_t end, time_t start) {
    return (double)(end - start);
}
