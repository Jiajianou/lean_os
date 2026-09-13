#pragma once

#include <stdint.h>

#include "syscall.h"

typedef struct {
    uint64_t calls;
    uint64_t cycles;
} syscount_entry_t;

void syscount_record(int num, uint64_t cycles);

int syscount_set_timing(int on);
int syscount_timing_enabled(void);

void syscount_reset(void);

void syscount_get(int num, syscount_entry_t *out);

uint64_t syscount_total_calls(void);
