#pragma once

#include <stdint.h>

static inline uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t)hi << 32) | lo;
}

void tsc_init(void);

uint64_t tsc_to_us(uint64_t cycles);

uint64_t tsc_cycles_per_us(void);
