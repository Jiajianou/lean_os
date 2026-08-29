/* kernel/arch/x86_64/tsc.h
 *
 * M69: a clock fine enough to measure latency with.
 *
 * Every timing question this project has asked so far has been answered
 * with the PIT, which ticks at PIT_HZ (100 Hz, 10 ms). That is the right
 * granularity for a scheduler quantum and for "has half a second passed",
 * and it is useless for the question M69 exists to answer. Input-to-photon
 * on a desktop is a 16 ms budget: measuring it with a 10 ms clock gives
 * you "one tick or two", which cannot distinguish a good frame from a bad
 * one and certainly cannot tell you whether a change helped.
 *
 * The Time Stamp Counter is the obvious instrument - one instruction, no
 * I/O ports, and a resolution of a CPU cycle. What it is not is a
 * *wall clock*, and the caveats are worth stating rather than discovering:
 *
 *  - it counts cycles, so it has to be calibrated against something that
 *    knows what a second is. That something is the PIT, which is the only
 *    real clock this kernel has.
 *  - on old hardware it stops in deep C-states and changes rate with
 *    frequency scaling. Modern CPUs have an invariant TSC and QEMU always
 *    does; this is checked at init and reported rather than assumed.
 *  - it is not guaranteed synchronised across cores. Every use here
 *    measures a duration on a single core within a single self-test, so
 *    that does not matter - but a caller that timed a start on one CPU
 *    and an end on another would be measuring skew, so don't.
 *
 * Deliberately not used for anything but measurement. The scheduler,
 * timeouts and pit_sleep_ms all still run off the PIT: replacing the
 * machine's timebase is a much larger change than measuring with a second
 * one, and nothing here needs it.
 */
#pragma once

#include <stdint.h>

/* Reads the counter. Serialised, so it cannot drift across the code being
 * measured - an unserialised RDTSC is free to execute early or late and
 * will happily report a negative interval for a short one. */
static inline uint64_t tsc_read(void) {
    uint32_t lo, hi;
    /* lfence rather than cpuid: cpuid is a VM exit under some hypervisors,
     * which is a large and variable cost to add to the thing being
     * measured. lfence orders against prior loads and, on every CPU this
     * kernel runs on, is enough to stop the RDTSC being hoisted. */
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Measures the TSC against the PIT and records cycles-per-microsecond.
 * Must run after pit_init and before any tsc_us call. Logs what it found,
 * including whether the CPU claims an invariant TSC. */
void tsc_init(void);

/* Converts a raw TSC delta to microseconds. Returns 0 before tsc_init. */
uint64_t tsc_to_us(uint64_t cycles);

/* Cycles per microsecond, for a caller that wants to report the
 * calibration itself. 0 before tsc_init. */
uint64_t tsc_cycles_per_us(void);
