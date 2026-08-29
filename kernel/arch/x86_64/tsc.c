#include "tsc.h"

#include "cpu.h"
#include "drivers/klog.h"
#include "drivers/pit.h"

static uint64_t cycles_per_us;

uint64_t tsc_cycles_per_us(void) {
    return cycles_per_us;
}

uint64_t tsc_to_us(uint64_t cycles) {
    if (cycles_per_us == 0) {
        return 0;
    }
    return cycles / cycles_per_us;
}

/* CPUID leaf 0x80000007, EDX bit 8: the TSC ticks at a constant rate and
 * does not stop in low-power states. Reported rather than required - a
 * machine without it still measures usefully over the microsecond
 * intervals this is used for, it just should not be trusted over minutes,
 * and saying which one you have beats assuming. */
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
    /* Calibrate over a whole number of PIT ticks, measured from one tick
     * boundary to another rather than from "now": starting mid-tick would
     * fold up to one tick of error (10 ms, i.e. 10%) into the result. */
    uint64_t start_tick = pit_get_ticks();
    while (pit_get_ticks() == start_tick) {
        /* spin to the next boundary */
    }
    uint64_t t0 = tsc_read();
    uint64_t from = pit_get_ticks();
    const uint64_t TICKS = 20; /* 200 ms - long enough that a tick of jitter is 0.5% */
    while (pit_get_ticks() - from < TICKS) {
        /* spin */
    }
    uint64_t elapsed = tsc_read() - t0;

    uint64_t us = TICKS * (1000000ull / PIT_HZ);
    cycles_per_us = elapsed / us;
    if (cycles_per_us == 0) {
        cycles_per_us = 1; /* refuse to divide by zero later; a machine this slow is not real */
    }

    klog_puts("[tsc] calibrated: ");
    klog_put_dec((uint32_t)cycles_per_us);
    klog_puts(" cycles/us (~");
    /* cycles/us IS MHz - dividing by 1000 again was a thinko that printed
     * 0 for every CPU slower than 1 GHz. */
    klog_put_dec((uint32_t)cycles_per_us);
    klog_puts(" MHz), invariant=");
    klog_put_dec((uint32_t)tsc_is_invariant());
    klog_putc('\n');
}
