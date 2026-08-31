#include "rtc.h"

#include "arch/x86_64/io.h"
#include "klog.h"
#include "pit.h" /* M85: the tick count the fallback clock extrapolates from */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define CMOS_SECONDS 0x00
#define CMOS_MINUTES 0x02
#define CMOS_HOURS   0x04
#define CMOS_DAY     0x07
#define CMOS_MONTH   0x08
#define CMOS_YEAR    0x09
#define CMOS_CENTURY 0x32
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

#define STATUS_A_UPDATE_IN_PROGRESS 0x80
#define STATUS_B_BINARY             0x04 /* clear = values are BCD */
#define STATUS_B_24_HOUR            0x02 /* clear = 12-hour, PM flagged in bit 7 of the hours register */

static int available;

/* Bit 7 of the index port is the NMI-disable bit on most chipsets.
 * Leaving it clear (which is what writing a bare register index does)
 * keeps NMIs enabled, which is what every other access in this kernel
 * assumes - deliberately not touched here. */
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static int update_in_progress(void) {
    return (cmos_read(CMOS_STATUS_A) & STATUS_A_UPDATE_IN_PROGRESS) != 0;
}

/* One coherent sample. The chip rolls its registers over once a second
 * and a read landing inside that rollover can see 10:59:59 becoming
 * 11:00:00 half-way - which is not a value that ever existed.
 *
 * The standard fix, and the reason this is not just seven inb's: wait
 * out any update in progress, take a full reading, take a second one,
 * and accept it only when the two agree. A disagreement means the update
 * happened between them, so it loops. Bounded rather than a `while (1)`:
 * a machine with no RTC at all reads 0xFF from every register, which
 * never settles, and hanging the boot over a missing clock would be the
 * wrong trade for a feature whose whole failure mode is "files are dated
 * zero, like they were yesterday".
 */
static int sample(os_datetime_t *out) {
    uint8_t last_sec = 0xFF, last_min = 0xFF, last_hour = 0xFF;
    uint8_t last_day = 0xFF, last_mon = 0xFF, last_year = 0xFF, last_cent = 0xFF;

    /* M85: 16 -> 64. Each attempt is two CMOS reads and the pair has to
     * agree; a host that deschedules this guest between them makes them
     * disagree, and sixteen tries was not enough headroom for a machine
     * running several guests at once. Cheap - a successful sample takes
     * two attempts - and the guard below is still what stops a dead chip
     * from hanging the boot. */
    for (int attempt = 0; attempt < 64; attempt++) {
        int guard = 0;
        while (update_in_progress() && guard++ < 1000000) {
            /* spin - one second at most on real hardware, and the guard
             * is what keeps a dead chip from being a hung boot */
        }
        uint8_t sec = cmos_read(CMOS_SECONDS);
        uint8_t min = cmos_read(CMOS_MINUTES);
        uint8_t hour = cmos_read(CMOS_HOURS);
        uint8_t day = cmos_read(CMOS_DAY);
        uint8_t mon = cmos_read(CMOS_MONTH);
        uint8_t year = cmos_read(CMOS_YEAR);
        uint8_t cent = cmos_read(CMOS_CENTURY);

        if (attempt > 0 && sec == last_sec && min == last_min && hour == last_hour &&
            day == last_day && mon == last_mon && year == last_year && cent == last_cent) {
            uint8_t status_b = cmos_read(CMOS_STATUS_B);
            int pm = 0;

            if (!(status_b & STATUS_B_24_HOUR)) {
                /* 12-hour mode flags PM in the top bit of the hours
                 * register - and it has to come off *before* the BCD
                 * conversion, or 0x92 (12-hour 12 PM) decodes as 92. */
                pm = (hour & 0x80) != 0;
                hour &= 0x7Fu;
            }
            if (!(status_b & STATUS_B_BINARY)) {
                sec = from_bcd(sec);
                min = from_bcd(min);
                hour = from_bcd(hour);
                day = from_bcd(day);
                mon = from_bcd(mon);
                year = from_bcd(year);
                cent = from_bcd(cent);
            }
            if (!(status_b & STATUS_B_24_HOUR)) {
                if (pm && hour < 12) {
                    hour = (uint8_t)(hour + 12);
                } else if (!pm && hour == 12) {
                    hour = 0;
                }
            }

            /* The century register is optional and reads as garbage on
             * chipsets that do not implement it, so it is used only when
             * it looks like a century. Otherwise the two-digit year is
             * windowed the way every BIOS does it - which is a guess, but
             * a bounded and well-understood one. */
            uint16_t full_year;
            if (cent >= 19 && cent <= 21) {
                full_year = (uint16_t)(cent * 100 + year);
            } else {
                full_year = (uint16_t)(year >= 70 ? 1900 + year : 2000 + year);
            }

            if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
                hour > 23 || min > 59 || sec > 60 || full_year < 1970 || full_year > 2200) {
                return 0; /* a reading this far out is not a clock, whatever it is */
            }
            out->year = full_year;
            out->month = mon;
            out->day = day;
            out->hour = hour;
            out->minute = min;
            out->second = sec;
            out->valid = 1;
            return 1;
        }
        last_sec = sec;
        last_min = min;
        last_hour = hour;
        last_day = day;
        last_mon = mon;
        last_year = year;
        last_cent = cent;
    }
    return 0;
}

void rtc_init(void) {
    os_datetime_t now;
    available = sample(&now);
    if (!available) {
        klog_puts("[rtc] no readable CMOS clock - files will be dated zero and the clock shows uptime.\n");
        return;
    }
    klog_puts("[rtc] ");
    klog_put_dec_pad(now.year, 4);
    klog_putc('-');
    klog_put_dec_pad(now.month, 2);
    klog_putc('-');
    klog_put_dec_pad(now.day, 2);
    klog_putc(' ');
    klog_put_dec_pad(now.hour, 2);
    klog_putc(':');
    klog_put_dec_pad(now.minute, 2);
    klog_putc(':');
    klog_put_dec_pad(now.second, 2);
    klog_puts(" (UTC, as the firmware keeps it) - this machine knows the date.\n");
}

int rtc_available(void) {
    return available;
}

/* M64: how far the CMOS clock is from the truth, in seconds, as told to
 * us by something that knows better (SNTP, via SYS_settime). Applied on
 * every read rather than written back to the hardware - see rtc.h. */
static int32_t correction;

/* ---- M85: a clock that keeps answering ---------------------------------
 *
 * `sample` accepts a reading only when two consecutive passes agree,
 * which is the right way to avoid reading a half-updated CMOS clock and
 * is exactly why it can fail: if this machine is descheduled *by its
 * host* between the two passes, the seconds field has moved on and the
 * readings never agree. Sixteen attempts later it gives up and reports
 * the time as invalid, and sys_time turns an invalid time into 0.
 *
 * That is not a hypothetical. It presented as the M63 self-test failing
 * with Whetstone's "Insufficient duration - Increase the LOOP count",
 * which is what that benchmark prints when its start and end timestamps
 * are equal - and they were equal because both were zero. It happened
 * only when several QEMU guests were competing for the host, which is
 * why it looked like a timing flake for a long time before it looked
 * like a clock bug. The self-test was already passing a loop count
 * chosen to "cross several whole seconds"; no loop count survives a
 * clock that has stopped.
 *
 * So a failed sample now extrapolates from the last good one using the
 * PIT tick count, rather than reporting no time at all. A clock that is
 * a fraction of a second stale under load is better than one that
 * answers zero, and much better than one that answers zero *silently* -
 * every caller here treats `valid` as a formality because until now it
 * was one.
 *
 * The extrapolation is not a fallback clock in the sense of a second
 * source of truth: the moment a real sample succeeds it takes over
 * again, and the baseline moves with it. */
static os_datetime_t last_good;
static uint64_t last_good_ticks;
static int have_good;

void rtc_read(os_datetime_t *out) {
    if (available && sample(out)) {
        if (correction != 0) {
            os_civil_from_unix((uint32_t)((int64_t)os_unix_time(out) + correction), out);
            out->valid = 1;
        }
        last_good = *out;
        last_good_ticks = pit_get_ticks();
        have_good = 1;
        return;
    }

    if (have_good) {
        uint64_t elapsed_ms = (pit_get_ticks() - last_good_ticks) * (1000 / PIT_HZ);
        os_civil_from_unix((uint32_t)(os_unix_time(&last_good) + (uint32_t)(elapsed_ms / 1000)),
                            out);
        out->valid = 1;
        return;
    }

    out->year = 0;
    out->month = 0;
    out->day = 0;
    out->hour = 0;
    out->minute = 0;
    out->second = 0;
    out->valid = 0;
}

int rtc_set_unix(uint32_t seconds) {
    /* Refuse anything before this project existed or absurdly far ahead:
     * a bad SNTP reply or a typo'd argument that moved every file's mtime
     * to 1904 is a much worse outcome than a clock that stays wrong, and
     * the check costs two comparisons. 2020-01-01 to 2100-01-01. */
    if (seconds < 1577836800u || seconds > 4102444800u) {
        return -1;
    }
    if (!available) {
        return -1;
    }
    os_datetime_t hw;
    if (!sample(&hw)) {
        return -1;
    }
    correction = (int32_t)((int64_t)seconds - (int64_t)os_unix_time(&hw));
    return 0;
}

uint32_t rtc_now(void) {
    os_datetime_t now;
    rtc_read(&now);
    if (!now.valid) {
        return 0;
    }
    return os_unix_time(&now);
}
