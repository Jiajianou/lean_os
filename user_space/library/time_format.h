#pragma once

#include <stdint.h>

#define TIME_FORMAT_CLOCK_MAX  12
#define TIME_FORMAT_OFFSET_MAX 12
#define TIME_FORMAT_DATE_MAX   40

#define TIME_FORMAT_OFFSET_MINIMUM (-12 * 60)
#define TIME_FORMAT_OFFSET_MAXIMUM (14 * 60)

int32_t time_format_clamp_offset(int32_t offset_minutes);

uint32_t time_format_local(uint32_t unix_seconds, int32_t offset_minutes);

void time_format_clock(uint32_t unix_seconds, int32_t offset_minutes, int twenty_four_hour,
                       char *out);

void time_format_offset(int32_t offset_minutes, char *out);

void time_format_date(uint32_t when, uint32_t now, int32_t offset_minutes, int twenty_four_hour,
                      char *out);

void time_format_long_date(uint32_t unix_seconds, int32_t offset_minutes, char *out);
