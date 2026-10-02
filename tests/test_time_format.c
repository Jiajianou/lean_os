#include "check.h"

#include "../user_space/library/time_format.h"

#include "os_time.h"

static uint32_t at(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute) {
    os_datetime_t t = {year, month, day, hour, minute, 0, 1};
    return os_unix_time(&t);
}

TEST(time_format, a_twenty_four_hour_clock_pads_and_a_twelve_hour_one_does_not) {
    char out[TIME_FORMAT_CLOCK_MAX];
    time_format_clock(at(2026, 10, 2, 9, 5), 0, 1, out);
    CHECK_STREQ(out, "09:05");
    time_format_clock(at(2026, 10, 2, 9, 5), 0, 0, out);
    CHECK_STREQ(out, "9:05 AM");
    time_format_clock(at(2026, 10, 2, 0, 30), 0, 0, out);
    CHECK_STREQ(out, "12:30 AM");
    time_format_clock(at(2026, 10, 2, 12, 0), 0, 0, out);
    CHECK_STREQ(out, "12:00 PM");
    time_format_clock(at(2026, 10, 2, 23, 59), 0, 0, out);
    CHECK_STREQ(out, "11:59 PM");
}

TEST(time_format, an_offset_moves_the_clock_across_midnight) {
    char out[TIME_FORMAT_CLOCK_MAX];
    time_format_clock(at(2026, 10, 2, 23, 0), 330, 1, out);
    CHECK_STREQ(out, "04:30");
    time_format_clock(at(2026, 10, 2, 2, 0), -300, 1, out);
    CHECK_STREQ(out, "21:00");
}

TEST(time_format, an_offset_is_written_the_way_people_write_it) {
    char out[TIME_FORMAT_OFFSET_MAX];
    time_format_offset(0, out);
    CHECK_STREQ(out, "UTC");
    time_format_offset(330, out);
    CHECK_STREQ(out, "UTC+05:30");
    time_format_offset(-210, out);
    CHECK_STREQ(out, "UTC-03:30");
    time_format_offset(100000, out);
    CHECK_STREQ(out, "UTC+14:00");
}

TEST(time_format, today_and_yesterday_are_named_and_older_days_are_dated) {
    char out[TIME_FORMAT_DATE_MAX];
    uint32_t now = at(2026, 10, 2, 15, 0);
    time_format_date(at(2026, 10, 2, 8, 15), now, 0, 1, out);
    CHECK_STREQ(out, "Today 08:15");
    time_format_date(at(2026, 10, 1, 22, 0), now, 0, 1, out);
    CHECK_STREQ(out, "Yesterday 22:00");
    time_format_date(at(2026, 9, 3, 7, 0), now, 0, 0, out);
    CHECK_STREQ(out, "3 Sep 2026 7:00 AM");
    time_format_date(0, now, 0, 1, out);
    CHECK_STREQ(out, "--");
}

TEST(time_format, today_is_decided_in_local_time) {
    char out[TIME_FORMAT_DATE_MAX];
    uint32_t now = at(2026, 10, 2, 1, 0);
    time_format_date(at(2026, 10, 1, 23, 0), now, 0, 1, out);
    CHECK_STREQ(out, "Yesterday 23:00");
    time_format_date(at(2026, 10, 1, 23, 0), now, -120, 1, out);
    CHECK_STREQ(out, "Today 21:00");
}

TEST(time_format, a_long_date_names_the_weekday) {
    char out[TIME_FORMAT_DATE_MAX];
    time_format_long_date(at(2026, 10, 2, 12, 0), 0, out);
    CHECK_STREQ(out, "Friday 2 October 2026");
    time_format_long_date(at(1970, 1, 1, 0, 0), 0, out);
    CHECK_STREQ(out, "Thursday 1 January 1970");
}

TEST(time_format, the_last_representable_second_does_not_wrap_to_1970) {
    char out[TIME_FORMAT_DATE_MAX];
    time_format_long_date(0xFFFFFF00u, 14 * 60, out);
    CHECK_STREQ(out, "Sunday 7 February 2106");
    time_format_long_date(30, -60, out);
    CHECK_STREQ(out, "Thursday 1 January 1970");
}

TEST(time_format, a_clock_that_does_not_know_today_never_says_today) {
    char out[TIME_FORMAT_DATE_MAX];
    time_format_date(5 * 3600, 0, 0, 1, out);
    CHECK_STREQ(out, "1 Jan 1970 05:00");
}
