#include "time_format.h"

#include "os_time.h"

static const char *const MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static const char *const LONG_MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

static const char *const WEEKDAYS[7] = {
    "Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday",
};

int32_t time_format_clamp_offset(int32_t offset_minutes) {
    if (offset_minutes < TIME_FORMAT_OFFSET_MINIMUM) {
        return TIME_FORMAT_OFFSET_MINIMUM;
    }
    if (offset_minutes > TIME_FORMAT_OFFSET_MAXIMUM) {
        return TIME_FORMAT_OFFSET_MAXIMUM;
    }
    return offset_minutes;
}

uint32_t time_format_local(uint32_t unix_seconds, int32_t offset_minutes) {
    int64_t shifted = (int64_t)unix_seconds + (int64_t)time_format_clamp_offset(offset_minutes) * 60;
    if (shifted < 0) {
        return 0;
    }
    if (shifted > 0xFFFFFFFFll) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)shifted;
}

static int put_text(char *out, int n, const char *text) {
    while (*text) {
        out[n++] = *text++;
    }
    out[n] = '\0';
    return n;
}

static int put_number(char *out, int n, uint32_t value, int width) {
    char digits[12];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);
    while (count < width) {
        digits[count++] = '0';
    }
    while (count > 0) {
        out[n++] = digits[--count];
    }
    out[n] = '\0';
    return n;
}

static int put_clock(char *out, int n, const os_datetime_t *t, int twenty_four_hour) {
    if (twenty_four_hour) {
        n = put_number(out, n, t->hour, 2);
        out[n++] = ':';
        return put_number(out, n, t->minute, 2);
    }
    uint32_t hour = t->hour % 12;
    n = put_number(out, n, hour == 0 ? 12 : hour, 1);
    out[n++] = ':';
    n = put_number(out, n, t->minute, 2);
    return put_text(out, n, t->hour < 12 ? " AM" : " PM");
}

void time_format_clock(uint32_t unix_seconds, int32_t offset_minutes, int twenty_four_hour,
                       char *out) {
    os_datetime_t t;
    os_civil_from_unix(time_format_local(unix_seconds, offset_minutes), &t);
    out[0] = '\0';
    put_clock(out, 0, &t, twenty_four_hour);
}

void time_format_offset(int32_t offset_minutes, char *out) {
    int32_t offset = time_format_clamp_offset(offset_minutes);
    int n = put_text(out, 0, "UTC");
    if (offset == 0) {
        return;
    }
    out[n++] = offset < 0 ? '-' : '+';
    uint32_t magnitude = (uint32_t)(offset < 0 ? -offset : offset);
    n = put_number(out, n, magnitude / 60, 2);
    out[n++] = ':';
    put_number(out, n, magnitude % 60, 2);
}

void time_format_date(uint32_t when, uint32_t now, int32_t offset_minutes, int twenty_four_hour,
                      char *out) {
    out[0] = '\0';
    if (when == 0) {
        put_text(out, 0, "--");
        return;
    }
    uint32_t local_when = time_format_local(when, offset_minutes);
    uint32_t local_now = time_format_local(now, offset_minutes);
    os_datetime_t t;
    os_civil_from_unix(local_when, &t);
    uint32_t day_when = local_when / 86400u;
    uint32_t day_now = local_now / 86400u;
    int n = 0;
    if (now != 0 && day_when == day_now) {
        n = put_text(out, n, "Today ");
    } else if (now != 0 && day_when + 1 == day_now) {
        n = put_text(out, n, "Yesterday ");
    } else {
        n = put_number(out, n, t.day, 1);
        out[n++] = ' ';
        n = put_text(out, n, MONTHS[(t.month + 11) % 12]);
        out[n++] = ' ';
        n = put_number(out, n, t.year, 4);
        out[n++] = ' ';
    }
    put_clock(out, n, &t, twenty_four_hour);
}

void time_format_long_date(uint32_t unix_seconds, int32_t offset_minutes, char *out) {
    uint32_t local = time_format_local(unix_seconds, offset_minutes);
    os_datetime_t t;
    os_civil_from_unix(local, &t);
    int n = put_text(out, 0, WEEKDAYS[(local / 86400u) % 7]);
    out[n++] = ' ';
    n = put_number(out, n, t.day, 1);
    out[n++] = ' ';
    n = put_text(out, n, LONG_MONTHS[(t.month + 11) % 12]);
    out[n++] = ' ';
    put_number(out, n, t.year, 4);
}
