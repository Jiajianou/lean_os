/* tests/fakes/fake_rtc.c - Q3
 *
 * A clock that does not move unless a test moves it.
 *
 * leanfs stamps every inode it writes with rtc_now(). A real clock makes
 * two runs of the same test produce two different disk images, which
 * rules out the one property a filesystem test most wants: that the same
 * sequence of operations produces the same bytes. So this one is a
 * counter, and fake_rtc_set() is how a test that cares about mtime
 * ordering gets one. */
#include "drivers/rtc.h"

#include <string.h>

static uint32_t now = 1000000000u; /* an arbitrary fixed epoch, 2001-09-09 */

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
