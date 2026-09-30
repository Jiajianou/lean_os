#include "timestamp_counter.h"

#include "cpu.h"
#include "drivers/kernel_log.h"
#include "drivers/pit.h"

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

void tsc_init(void) {
    uint64_t start_tick = pit_get_ticks();
    while (pit_get_ticks() == start_tick) {
    }
    uint64_t t0 = tsc_read();
    uint64_t from = pit_get_ticks();
    const uint64_t TICKS = 20;
    while (pit_get_ticks() - from < TICKS) {
    }
    uint64_t elapsed = tsc_read() - t0;

    uint64_t us = TICKS * (1000000ull / PIT_HZ);
    cycles_per_us = elapsed / us;
    if (cycles_per_us == 0) {
        cycles_per_us = 1;
    }
    if (elapsed > 0) {
        calibrated_tsc = t0 + elapsed;
        calibrated_ns = pit_get_ticks() * (1000000000ULL / PIT_HZ);
        ns_per_cycle_q24 = ((us * 1000ULL) << 24) / elapsed;
    }

    kernel_log_puts("[tsc] calibrated: ");
    kernel_log_put_dec((uint32_t)cycles_per_us);
    kernel_log_puts(" cycles/us (~");
    kernel_log_put_dec((uint32_t)cycles_per_us);
    kernel_log_puts(" MHz), invariant=");
    kernel_log_put_dec((uint32_t)tsc_is_invariant());
    kernel_log_putc('\n');
}
