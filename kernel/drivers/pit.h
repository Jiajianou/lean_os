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
