#include "rtc.h"

#include "architecture/x86_64/io.h"
#include "kernel_log.h"
#include "architecture/x86_64/timestamp_counter.h"
#include "library/spinlock.h"

#define CMOS_ADDRESS 0x70
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
#define STATUS_B_BINARY             0x04
#define STATUS_B_24_HOUR            0x02

static int available;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static int update_in_progress(void) {
    return (cmos_read(CMOS_STATUS_A) & STATUS_A_UPDATE_IN_PROGRESS) != 0;
}

static int sample(os_datetime_t *out) {
    uint8_t last_sec = 0xFF, last_min = 0xFF, last_hour = 0xFF;
    uint8_t last_day = 0xFF, last_mon = 0xFF, last_year = 0xFF, last_cent = 0xFF;

    for (int attempt = 0; attempt < 64; attempt++) {
        int guard = 0;
        while (update_in_progress() && guard++ < 1000000) {
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

            uint16_t full_year;
            if (cent >= 19 && cent <= 21) {
                full_year = (uint16_t)(cent * 100 + year);
            } else {
                full_year = (uint16_t)(year >= 70 ? 1900 + year : 2000 + year);
            }

            if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
                hour > 23 || min > 59 || sec > 60 || full_year < 1970 || full_year > 2200) {
                return 0;
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
        kernel_log_puts("[rtc] no readable CMOS clock - files will be dated zero and the clock shows uptime.\n");
        return;
    }
    kernel_log_puts("[rtc] ");
    kernel_log_put_dec_pad(now.year, 4);
    kernel_log_putc('-');
    kernel_log_put_dec_pad(now.month, 2);
    kernel_log_putc('-');
    kernel_log_put_dec_pad(now.day, 2);
    kernel_log_putc(' ');
    kernel_log_put_dec_pad(now.hour, 2);
    kernel_log_putc(':');
    kernel_log_put_dec_pad(now.minute, 2);
    kernel_log_putc(':');
    kernel_log_put_dec_pad(now.second, 2);
    kernel_log_puts(" (UTC, as the firmware keeps it) - this machine knows the date.\n");
}

int rtc_available(void) {
    return available;
}

static int32_t correction;

static os_datetime_t last_good;
static uint64_t last_good_tsc;
static int have_good;

/* M203: the CMOS clock is read once a minute, not once a call. Every
   gettimeofday and CLOCK_REALTIME a program made was a SYS_time, and each
   was two passes over eight I/O ports - about a microsecond apiece on real
   hardware - with no lock, so two cores reading at once overwrote each
   other's index register, the passes disagreed, and it tried again, up to
   sixty-four times. Chromium asks for the wall clock from many threads at
   once: on the laptop that was most of a processor while the browser
   started. Between samples the answer is the last sample plus the TSC's
   elapsed seconds, which is what this function already fell back to when a
   sample failed; a new sample moves the anchor only when the two disagree
   by more than a second, so a reader never sees the time jitter by one at
   each minute. */
#define RTC_RESAMPLE_US 60000000ull

static spinlock_t rtc_lock;
static uint64_t cmos_samples;
static uint64_t last_sample_tsc;

static uint32_t extrapolated_locked(uint64_t now_tsc) {
    uint64_t elapsed_s = tsc_to_us(now_tsc - last_good_tsc) / 1000000ULL;
    return os_unix_time(&last_good) + (uint32_t)elapsed_s;
}

void rtc_read(os_datetime_t *out) {
    uint64_t flags = spin_lock_irqsave(&rtc_lock);
    uint64_t now_tsc = tsc_read();
    if (available && (!have_good || tsc_to_us(now_tsc - last_sample_tsc) >= RTC_RESAMPLE_US)) {
        os_datetime_t fresh;
        cmos_samples++;
        last_sample_tsc = now_tsc;
        if (sample(&fresh)) {
            if (correction != 0) {
                os_civil_from_unix((uint32_t)((int64_t)os_unix_time(&fresh) + correction), &fresh);
                fresh.valid = 1;
            }
            int64_t drift = have_good ? (int64_t)os_unix_time(&fresh) - (int64_t)extrapolated_locked(now_tsc) : 0;
            if (!have_good || drift > 1 || drift < -1) {
                last_good = fresh;
                last_good_tsc = now_tsc;
            }
            have_good = 1;
        }
    }

    if (have_good) {
        os_civil_from_unix(extrapolated_locked(now_tsc), out);
        out->valid = 1;
        spin_unlock_irqrestore(&rtc_lock, flags);
        return;
    }
    spin_unlock_irqrestore(&rtc_lock, flags);

    out->year = 0;
    out->month = 0;
    out->day = 0;
    out->hour = 0;
    out->minute = 0;
    out->second = 0;
    out->valid = 0;
}

uint64_t rtc_cmos_samples(void) {
    return __atomic_load_n(&cmos_samples, __ATOMIC_RELAXED);
}

int rtc_set_unix(uint32_t seconds) {
    if (seconds < 1577836800u || seconds > 4102444800u) {
        return -1;
    }
    if (!available) {
        return -1;
    }
    uint64_t flags = spin_lock_irqsave(&rtc_lock);
    os_datetime_t hw;
    cmos_samples++;
    if (!sample(&hw)) {
        spin_unlock_irqrestore(&rtc_lock, flags);
        return -1;
    }
    correction = (int32_t)((int64_t)seconds - (int64_t)os_unix_time(&hw));
    os_civil_from_unix(seconds, &last_good);
    last_good.valid = 1;
    last_good_tsc = tsc_read();
    last_sample_tsc = last_good_tsc;
    have_good = 1;
    spin_unlock_irqrestore(&rtc_lock, flags);
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
