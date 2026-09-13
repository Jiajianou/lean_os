#include "drivers/rtc.h"

#include <string.h>

static uint32_t now = 1000000000u;

void fake_rtc_set(uint32_t t);
void fake_rtc_advance(uint32_t secs);

void fake_rtc_set(uint32_t t) { now = t; }
void fake_rtc_advance(uint32_t secs) { now += secs; }

void rtc_init(void) {}
uint32_t rtc_now(void) { return now; }
int rtc_available(void) { return 1; }
int rtc_set_unix(uint32_t seconds) { now = seconds; return 0; }

void rtc_read(os_datetime_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}
