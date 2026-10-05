#pragma once

#include <stdint.h>

/* The periodic timer interrupt as a clock - and the rule that keeps it one
   when the interrupt controller delivers more edges than the timer made.

   QEMU's PIT re-asserts the level it already has at every transition of
   its output: traced (-trace ioapic_set_irq,pic_set_irq), each transition
   is the OLD level and then the new one a few microseconds apart - 1,0 at
   the fall and 0,1 at the rise - which looks like its nanosecond deadline
   being rounded down so the first look still sees the old level. The 8259
   remembers the line's level and finds one rising edge per period in
   that: 100 interrupts a second. QEMU's I/O APIC does not: every
   assertion on an edge-triggered pin is a new interrupt, so the timer's
   vector fires twice a period, 200 a second, under TCG and under hvf
   alike, because both use the same device models. A real I/O APIC detects
   edges and should not be affected; a kernel that counts interrupts as
   time is, and before this every clock here ran at twice the rate on that
   path (the TSC calibrated at half its frequency, the LAPIC timer with
   it, and every measured millisecond doubled).

   The rule: once the TSC has been calibrated against something that is not
   an interrupt (pit.c's channel 2, polled), an interrupt is a tick only if
   time says one is due - the tick count may never run more than a quarter
   period ahead of the TSC. Each period's rising edge is then a tick and the
   echo half a period later is not; a machine whose controller delivers one
   interrupt per period loses nothing, because its interrupts are a whole
   period apart. */

typedef struct {
    uint64_t epoch_tsc;        /* the TSC at a tick that was counted */
    uint64_t epoch_ticks;      /* the tick count at that moment */
    uint64_t cycles_per_tick;  /* 0 until calibrated: every interrupt counts */
} tick_clock_t;

/* Whether an interrupt that arrives at now_tsc, with the count at ticks,
   is a tick. */
int tick_clock_accept(const tick_clock_t *clock, uint64_t ticks, uint64_t now_tsc);

/* What an independent clock (the RTC's second boundaries) says about the
   two this kernel keeps. All rates in thousandths of what they should be;
   1000 is exact. */
typedef struct {
    uint32_t reference_ms;       /* the interval, by the independent clock */
    uint64_t tsc_cycles;         /* the TSC over that interval */
    uint64_t cycles_per_us;      /* what calibration said the TSC runs at */
    uint64_t ticks;              /* timer ticks counted over the interval */
    uint64_t interrupts;         /* timer interrupts taken over the interval */
    uint32_t tick_hz;            /* the rate the timer was programmed for */
} timekeeping_sample_t;

typedef struct {
    uint32_t tsc_permille;        /* the calibrated TSC clock against the reference */
    uint32_t tick_permille;       /* the tick count against the reference */
    uint32_t interrupt_permille;  /* interrupts taken per programmed period */
    int ok;
} timekeeping_verdict_t;

/* Within TIMEKEEPING_TOLERANCE_PERMILLE of 1000 on both clocks is "keeps
   time". The tolerance is wide on purpose and is not a measurement budget:
   the failure it exists for is a factor of two, and under TCG on a busy
   host the tick loses the occasional interrupt QEMU could not deliver in
   time (a percent or two, measured), which is not this check's business. */
#define TIMEKEEPING_TOLERANCE_PERMILLE 100u

void timekeeping_grade(const timekeeping_sample_t *sample, timekeeping_verdict_t *out);

/* The middle of three measurements - one that a host stall lengthened or a
   late first read shortened is on the outside. */
uint64_t tick_clock_median3(uint64_t a, uint64_t b, uint64_t c);
