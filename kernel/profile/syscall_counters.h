#pragma once

#include <stdint.h>

#include "syscall.h"

typedef struct {
    uint64_t calls;
    uint64_t cycles;
} syscall_counters_entry_t;

void syscall_counters_record(int num, uint64_t cycles);

int syscall_counters_set_timing(int on);
int syscall_counters_timing_enabled(void);

void syscall_counters_reset(void);

void syscall_counters_get(int num, syscall_counters_entry_t *out);

