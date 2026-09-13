#include "rtc.h"

#include "architecture/x86_64/io.h"
#include "kernel_log.h"
#include "architecture/x86_64/timestamp_counter.h"

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
#define STATUS_B_BINARY             0x04
#define STATUS_B_24_HOUR            0x02

static int available;

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

static int32_t correction;

static os_datetime_t last_good;
static uint64_t last_good_tsc;
static int have_good;

void rtc_read(os_datetime_t *out) {
    if (available && sample(out)) {
        if (correction != 0) {
            os_civil_from_unix((uint32_t)((int64_t)os_unix_time(out) + correction), out);
            out->valid = 1;
        }
        last_good = *out;
        last_good_tsc = tsc_read();
        have_good = 1;
        return;
    }

    if (have_good) {
        uint64_t elapsed_s = tsc_to_us(tsc_read() - last_good_tsc) / 1000000ULL;
        os_civil_from_unix((uint32_t)(os_unix_time(&last_good) + (uint32_t)elapsed_s), out);
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
