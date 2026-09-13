#include "tsc.h"

#include "cpu.h"
#include "drivers/klog.h"
#include "drivers/pit.h"

static uint64_t cycles_per_us;

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

    klog_puts("[tsc] calibrated: ");
    klog_put_dec((uint32_t)cycles_per_us);
    klog_puts(" cycles/us (~");
    klog_put_dec((uint32_t)cycles_per_us);
    klog_puts(" MHz), invariant=");
    klog_put_dec((uint32_t)tsc_is_invariant());
    klog_putc('\n');
}
