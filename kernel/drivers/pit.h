#pragma once

#include <stdint.h>

#define PIT_HZ 100

void pit_init(void);
uint64_t pit_get_ticks(void);

void pit_sleep_ms(uint32_t ms);

void pit_set_tick_hook(void (*hook)(void));

/* Time since boot as every program sees it: the TSC, calibrated against this
   timer, so it reads to the nanosecond rather than advancing ten
   milliseconds at a time with the tick. Deadlines inside the kernel are kept
   in the same clock, so a program and the kernel agree about when "now" is;
   the tick only decides how often the kernel looks. */
uint64_t clock_monotonic_ns(void);
uint64_t clock_monotonic_ms(void);

/* M200: the deadline for a wait of timeout_ms that starts now - the first
   whole millisecond at or after now plus the timeout. Deadlines are kept in
   milliseconds, and "now" cut to a millisecond made every timed wait end up
   to a millisecond early, which POSIX does not allow and which a caller that
   checks the time sees as a spurious timeout to wait again for. */
uint64_t clock_deadline_ms(uint64_t timeout_ms);
