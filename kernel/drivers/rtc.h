#pragma once

#include <stdint.h>

#include "os_time.h"

void rtc_init(void);

void rtc_read(os_datetime_t *out);

uint32_t rtc_now(void);

int rtc_set_unix(uint32_t seconds);

int rtc_available(void);

uint64_t rtc_cmos_samples(void);

/* Waits for the CMOS clock's next second boundary and reports the TSC at
   it. 0 when there is no clock or no boundary came. */
int rtc_next_second_edge(uint64_t *tsc_at);
