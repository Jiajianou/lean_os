/* system_api/include/os_time.h
 *
 * M59: what a machine that knows the date can tell user space.
 *
 * Until this milestone every file in this OS was dated zero and the
 * taskbar clock counted uptime, because uptime was the only thing this
 * machine could know - there was no clock, only a tick counter started at
 * boot. A hundred lines of CMOS RTC turns three zeros into truths at
 * once: a file's mtime, a date column in the file manager, and a clock
 * that shows the time.
 *
 * Two representations, because the two callers want different things. A
 * `uint32_t` seconds-since-1970 is what gets stored in an inode and
 * compared when sorting; a broken-down `os_datetime_t` is what gets
 * drawn. The kernel converts, so there is one implementation of the
 * calendar rather than one in each program that wants a date.
 */
#pragma once

#include <stdint.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

/* M59: what SYS_stat reports. Deliberately not the kernel's own
 * leanfs_stat_t: that one is a filesystem's internal record and this one
 * is an ABI, and the day a second filesystem exists they stop being the
 * same shape. */
typedef struct {
    uint32_t size;
    uint32_t mtime;   /* seconds since 1970, or 0 if this file predates the machine knowing the date */
    uint8_t is_dir;
    /* M87: was `reserved`, and is now the field that lets a program tell
     * a symbolic link from what it points at.
     *
     * It takes the reserved byte rather than a new one on purpose: the
     * kernel copies leanfs_stat_t into this struct byte for byte, so the
     * two layouts have to agree exactly. Appending a field here instead
     * would have put is_link at offset 10 on this side and offset 9 on
     * the kernel's - a mismatch that reads as "every file is a link" or
     * "no file is", depending which way the padding fell.
     *
     * Only ever set by SYS_lstat: every other path call follows a link,
     * so by the time they answer there is nothing left to report. */
    uint8_t is_link;
    /* M89: the inode number, and the first thing on this machine that
     * makes two paths distinguishable as files.
     *
     * A program that compares (st_dev, st_ino) to decide "same file" -
     * which is every cp, mv, ln and find in the world - was getting a
     * yes for every pair, because both halves were 0. This is the half
     * that can be answered: leanfs knows which inode a path resolved to.
     * st_dev stays 0 and is the half that cannot, because there is one
     * filesystem here (see /proc/mounts); the synthetic mounts avoid
     * colliding with leanfs by numbering from a disjoint base rather
     * than by having a device number, which is written down in devfs.c
     * and procfs.c where the numbers are chosen. */
    uint32_t inode;
} os_stat_t;

typedef struct {
    uint16_t year;   /* full year, e.g. 2026 */
    uint8_t month;   /* 1-12 */
    uint8_t day;     /* 1-31 */
    uint8_t hour;    /* 0-23 */
    uint8_t minute;  /* 0-59 */
    uint8_t second;  /* 0-60 */
    uint8_t valid;   /* 0 if this machine has no readable RTC - see SYS_time */
} os_datetime_t;

/* Seconds from 1970-01-01T00:00:00 to the start of `year`, for the
 * Gregorian rules this project uses. Shared so that "what time is it"
 * and "what time was this file written" agree about the calendar. */
static inline uint32_t os_days_from_civil(uint16_t year, uint8_t month, uint8_t day) {
    /* Howard Hinnant's days-from-civil, in the integer-only form this
     * project's no-floating-point rule requires. Shifting the year to
     * start in March makes the leap day the last day of the year, which
     * is what removes every special case from the month-length table. */
    uint32_t y = year;
    uint32_t m = month;
    y -= m <= 2;
    uint32_t era = y / 400;
    uint32_t yoe = y - era * 400;                                  /* 0..399 */
    uint32_t doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u; /* 0..365 */
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* 0..146096 */
    return era * 146097u + doe - 719468u;                          /* days since 1970-01-01 */
}

static inline uint32_t os_unix_time(const os_datetime_t *t) {
    return os_days_from_civil(t->year, t->month, t->day) * 86400u +
           (uint32_t)t->hour * 3600u + (uint32_t)t->minute * 60u + t->second;
}

/* The inverse, for turning a stored mtime back into something drawable.
 * Same calendar, same file, so the round trip cannot drift. */
static inline void os_civil_from_unix(uint32_t secs, os_datetime_t *out) {
    uint32_t days = secs / 86400u;
    uint32_t rem = secs % 86400u;
    out->hour = (uint8_t)(rem / 3600u);
    out->minute = (uint8_t)((rem % 3600u) / 60u);
    out->second = (uint8_t)(rem % 60u);

    uint32_t z = days + 719468u;
    uint32_t era = z / 146097u;
    uint32_t doe = z - era * 146097u;                                          /* 0..146096 */
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;   /* 0..399 */
    uint32_t y = yoe + era * 400u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);                  /* 0..365 */
    uint32_t mp = (5u * doy + 2u) / 153u;                                       /* 0..11 */
    uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;                              /* 1..31 */
    uint32_t m = mp + (mp < 10u ? 3u : (uint32_t)-9);                           /* 1..12 */
    out->year = (uint16_t)(y + (m <= 2u));
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    out->valid = 1;
}

#ifdef __cplusplus
}
#endif
