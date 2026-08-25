/* user_space/libc/include/time.h - M63.
 *
 * Just enough for a program that wants to know how long it took: a
 * seconds-since-1970 clock over M59's CMOS RTC, and clock() over the
 * millisecond tick counter this OS has had since M6.
 *
 * Deliberately no struct tm, no strftime, no localtime. This machine has
 * no notion of a timezone - the RTC is read as UTC and said so - and
 * inventing one to satisfy a header would be inventing rather than
 * porting. A program that needs them will say so at link time, which is
 * the specification. */
#pragma once

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

/* M6's tick is 100 Hz, so this is the honest resolution rather than the
 * conventional 1000000. */
#define CLOCKS_PER_SEC 1000

time_t time(time_t *out);
clock_t clock(void);
