/* kernel/drivers/rtc.h
 *
 * M59: the CMOS real-time clock, on ports 0x70/0x71 - the one piece of
 * hardware in a PC that knows what day it is, and the reason every file
 * in this OS was dated zero until this milestone.
 *
 * About a hundred lines, and the single highest visibility-per-line
 * change available: it turns three zeros into truths at once - a file's
 * mtime, a date column in the file manager, and a taskbar clock that
 * shows the time instead of how long the machine has been switched on.
 *
 * Two things make reading it more than an `inb`: the chip updates its
 * registers once a second and a read landing inside that update returns a
 * torn value, and it reports BCD or binary depending on a status bit
 * nobody may change. Both are handled here rather than by callers.
 */
#pragma once

#include <stdint.h>

#include "os_time.h" /* system_api/include/os_time.h - os_datetime_t, shared with user space */

/* Reads the clock once at boot and reports what it found. A machine with
 * no readable RTC is an ordinary outcome, not a failure - `valid` is 0
 * and every timestamp this OS writes is then 0, exactly as it was before
 * this milestone. */
void rtc_init(void);

/* The current wall-clock time. Cheap enough to call per redraw (two port
 * writes and seven reads), so there is no cached-and-ticked copy to drift
 * from the hardware. */
void rtc_read(os_datetime_t *out);

/* The same instant as seconds since 1970-01-01, which is what an inode
 * stores. 0 if this machine has no readable clock, which is also what an
 * un-timestamped file carries - so "no clock" and "no timestamp" print
 * the same way, which is the honest outcome for both. */
uint32_t rtc_now(void);

/* M64: corrects the clock to `seconds` since 1970 - what SYS_settime is
 * for and what an SNTP client produces. Stores an offset applied on every
 * subsequent read rather than writing the CMOS registers: reprogramming
 * the hardware clock is something a machine's owner asks a boot utility
 * to do, and a program that happened to get the network working is not
 * the same thing as that owner's consent. The offset lasts until reboot,
 * which is the honest scope for a correction nothing persisted. Returns
 * 0, or -1 for an implausible time or a machine with no readable clock. */
int rtc_set_unix(uint32_t seconds);

/* 1 if the boot-time probe found a plausible clock. */
int rtc_available(void);
