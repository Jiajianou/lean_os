#pragma once

#include <stdint.h>

#define PIT_HZ 100

void pit_init(void);
uint64_t pit_get_ticks(void);

/* Every timer interrupt taken, and those of them that were not a tick
   because the TSC said the next tick was not due yet (tick_clock.h). Where
   the interrupt controller sees one edge per period the second stays near
   zero; under QEMU's I/O APIC it is half the first. */
uint64_t pit_get_interrupts(void);
uint64_t pit_get_echoes(void);

/* TSC cycles while channel 2 counts `counts` of the timer's own clock,
   polled - no interrupt involved. 0 if the channel never answered. */
int pit_channel2_cycles(uint16_t counts, uint64_t *cycles);

/* From here on, a timer interrupt is a tick only when the TSC says one is
   due: window_cycles TSC cycles were measured over window_counts of the
   timer's input clock, which is the clock channel 0 divides. */
void pit_start_tick_clock(uint64_t window_cycles, uint32_t window_counts);

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
