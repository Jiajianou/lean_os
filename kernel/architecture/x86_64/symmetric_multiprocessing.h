#pragma once

#include <stdint.h>

#include "cpu.h"

typedef struct {
    uint32_t apic_id;
    volatile int online;
} cpu_info_t;

extern int smp_cpu_count;

void smp_init(void);

int smp_current_cpu(void);

int smp_is_initialized(void);

void smp_broadcast_schedule_tick(void);

/* Discard every other core's cached translations and do not return until each
   of them has done it. A core that changes a live address space another core
   may be running in calls this after the change and before anything can
   depend on it - fork downgrading a page to copy-on-write is the case that
   asked for it, because the writable bit it clears is otherwise still in a
   sibling's TLB. */
void smp_tlb_shootdown(void);

void smp_tlb_shootdown_acknowledge(void);

void smp_halt_other_cpus(void);
int smp_halt_was_requested(void);
