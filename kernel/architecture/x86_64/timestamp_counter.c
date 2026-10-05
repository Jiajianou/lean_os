#include "timestamp_counter.h"

#include "cpu.h"
#include "drivers/kernel_log.h"
#include "drivers/pit.h"
#include "drivers/tick_clock.h"

static uint64_t cycles_per_us;

/* clock_monotonic_ns: nanoseconds per cycle in 8.24 fixed point, and where
   the TSC and the tick agreed at calibration. The multiply is split so that
   a machine up for months does not overflow 64 bits. */
static uint64_t ns_per_cycle_q24;
static uint64_t calibrated_tsc;
static uint64_t calibrated_ns;
static uint64_t last_reported_ns;

uint64_t clock_monotonic_ns(void) {
    uint64_t now;
    if (ns_per_cycle_q24 == 0) {
        now = pit_get_ticks() * (1000000000ULL / PIT_HZ);
    } else {
        uint64_t cycles = tsc_read() - calibrated_tsc;
        now = calibrated_ns + (cycles >> 24) * ns_per_cycle_q24 +
              (((cycles & 0xFFFFFFULL) * ns_per_cycle_q24) >> 24);
    }
    /* Each core reads its own TSC, and one that is a few cycles behind
       another must not make time run backwards for a thread that moved. */
    uint64_t seen = __atomic_load_n(&last_reported_ns, __ATOMIC_RELAXED);
    while (now > seen) {
        if (__atomic_compare_exchange_n(&last_reported_ns, &seen, now, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return now;
        }
    }
    return seen > now ? seen : now;
}

uint64_t clock_monotonic_ms(void) {
    return clock_monotonic_ns() / 1000000ULL;
}

uint64_t clock_deadline_ms(uint64_t timeout_ms) {
    return (clock_monotonic_ns() + timeout_ms * 1000000ULL + 999999ULL) / 1000000ULL;
}

uint64_t tsc_cycles_per_us(void) {
    return cycles_per_us;
}

uint64_t tsc_to_us(uint64_t cycles) {
    if (cycles_per_us == 0) {
        return 0;
    }
    return cycles / cycles_per_us;
}

static int tsc_is_invariant(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u));
    if (eax < 0x80000007u) {
        return 0;
    }
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000007u));
    return (edx & (1u << 8)) != 0;
}

/* 50 ms of the 8254's 1.193182 MHz input clock: channel 2's whole count
   fits in its 16 bits, and a 50 ms window puts a 1 us polling granularity
   at 20 parts per million. */
#define CALIBRATION_COUNTS 59659u
#define PIT_INPUT_HZ 1193182ull

void tsc_init(void) {
    /* What the timer INTERRUPT says the TSC runs at: twenty of them. This
       was the calibration until it was found to be the interrupt
       controller's opinion rather than the timer's - QEMU's I/O APIC takes
       the PIT's interrupt twice a period (drivers/tick_clock.h), and every
       clock here ran at double speed on that path. It is kept as the
       fallback for a machine whose channel 2 does not answer, and as the
       measurement of how many interrupts a period this controller
       delivers. */
    uint64_t start_interrupt = pit_get_interrupts();
    while (pit_get_interrupts() == start_interrupt) {
    }
    uint64_t t0 = tsc_read();
    uint64_t from = pit_get_interrupts();
    const uint64_t INTERRUPTS = 20;
    while (pit_get_interrupts() - from < INTERRUPTS) {
    }
    uint64_t by_interrupts = tsc_read() - t0;

    /* What the timer's own oscillator says: channel 2 counted down and
       polled, three times, the middle one kept. */
    uint64_t window[3];
    int answered = 0;
    while (answered < 3 && pit_channel2_cycles((uint16_t)CALIBRATION_COUNTS, &window[answered])) {
        answered++;
    }

    uint64_t elapsed;
    uint64_t window_ns;
    if (answered == 3) {
        elapsed = tick_clock_median3(window[0], window[1], window[2]);
        window_ns = CALIBRATION_COUNTS * 1000000000ull / PIT_INPUT_HZ;
    } else {
        elapsed = by_interrupts;
        window_ns = INTERRUPTS * (1000000000ull / PIT_HZ);
    }

    cycles_per_us = elapsed * 1000ull / window_ns;
    if (cycles_per_us == 0) {
        cycles_per_us = 1;
    }
    if (elapsed > 0) {
        calibrated_tsc = tsc_read();
        calibrated_ns = pit_get_ticks() * (1000000000ULL / PIT_HZ);
        ns_per_cycle_q24 = (window_ns << 24) / elapsed;
    }
    /* Interrupts per period, in hundredths: 100 is one edge per period. */
    uint64_t per_period = by_interrupts ? (elapsed * 1000ull / window_ns) * INTERRUPTS * 100ull *
                                              (1000000ull / PIT_HZ) / by_interrupts
                                        : 0;
    if (answered == 3) {
        pit_start_tick_clock(elapsed, CALIBRATION_COUNTS);
    }

    kernel_log_puts("[tsc] calibrated: ");
    kernel_log_put_dec((uint32_t)cycles_per_us);
    kernel_log_puts(" cycles/us (~");
    kernel_log_put_dec((uint32_t)cycles_per_us);
    kernel_log_puts(" MHz), invariant=");
    kernel_log_put_dec((uint32_t)tsc_is_invariant());
    if (answered == 3) {
        kernel_log_puts(", against PIT channel 2; the timer interrupt arrives ");
        kernel_log_put_dec((uint32_t)(per_period / 100));
        kernel_log_putc('.');
        kernel_log_put_dec_pad((uint32_t)(per_period % 100), 2);
        kernel_log_puts(" times a period, and the tick counts one");
    } else {
        kernel_log_puts(", against the timer interrupt - PIT channel 2 did not answer");
    }
    kernel_log_putc('\n');
}
