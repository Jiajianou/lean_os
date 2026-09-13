#pragma once

#include <stdint.h>

#include "cpu.h"

typedef struct {
    uint32_t apic_id;
    volatile int online;
} cpu_info_t;

extern cpu_info_t smp_cpus[MAX_CPUS];
extern int smp_cpu_count;

void smp_init(void);

int smp_current_cpu(void);

int smp_is_initialized(void);

void smp_broadcast_schedule_tick(void);

void smp_halt_other_cpus(void);
