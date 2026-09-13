#include "syscall_counters.h"

#include "library/kernel_library.h"

static syscall_counters_entry_t table[SYSCALL_COUNT];
static volatile int timing_on;

void syscall_counters_record(int num, uint64_t cycles) {
    if (num < 0 || num >= SYSCALL_COUNT) {
        return;
    }
    __atomic_fetch_add(&table[num].calls, 1, __ATOMIC_RELAXED);
    if (cycles) {
        __atomic_fetch_add(&table[num].cycles, cycles, __ATOMIC_RELAXED);
    }
}

int syscall_counters_set_timing(int on) {
    int was = timing_on;
    timing_on = on ? 1 : 0;
    return was;
}

int syscall_counters_timing_enabled(void) {
    return timing_on;
}

void syscall_counters_reset(void) {
    k_memset(table, 0, sizeof(table));
}

void syscall_counters_get(int num, syscall_counters_entry_t *out) {
    if (!out) {
        return;
    }
    if (num < 0 || num >= SYSCALL_COUNT) {
        out->calls = 0;
        out->cycles = 0;
        return;
    }
    out->calls = __atomic_load_n(&table[num].calls, __ATOMIC_RELAXED);
    out->cycles = __atomic_load_n(&table[num].cycles, __ATOMIC_RELAXED);
}
