#pragma once

#include <stdint.h>

#include "os_time.h"

void rtc_init(void);

void rtc_read(os_datetime_t *out);

uint32_t rtc_now(void);

int rtc_set_unix(uint32_t seconds);

int rtc_available(void);
