/* kernel/arch/x86_64/smp.h
 *
 * SMP bring-up: finds every enabled CPU via ACPI (kernel/acpi/acpi.h),
 * starts each one with the standard INIT-SIPI-SIPI sequence over its
 * Local APIC (lapic.h), and hands it off to the scheduler
 * (kernel/sched/sched.c's sched_init_ap) as just another core competing
 * for the same shared run queue - there's no per-CPU task affinity, any
 * online CPU can run any READY task.
 *
 * If ACPI/the MADT isn't present, smp_init() just logs that and leaves
 * the kernel running single-core (cpu 0, the BSP) exactly as it always
 * has - not a failure, a legitimate fallback.
 */
#pragma once

#include <stdint.h>

#include "cpu.h"

typedef struct {
    uint32_t apic_id;
    volatile int online; /* set by the AP itself (smp.c's ap_main), not by whoever started it */
} cpu_info_t;

extern cpu_info_t smp_cpus[MAX_CPUS];
extern int smp_cpu_count; /* informational only (logging) - see smp_current_cpu for why it's not load-bearing */

/* BSP-only, called once from kernel_main after the scheduler exists
 * (sched_init) - an AP becomes a real schedulable task the moment it
 * checks in, so the scheduler has to be ready to receive it. */
void smp_init(void);

/* Which slot in smp_cpus[]/sched.c's per-CPU arrays the *calling* CPU is -
 * looked up by this CPU's own Local APIC id every time (a plain MMIO
 * read), not cached in a per-CPU variable: this kernel has no per-CPU
 * storage mechanism (no swapgs/GS-base setup) cheap enough to be worth
 * adding just for this, and an MMIO read is more than fast enough at this
 * kernel's scale (see gdt.h/sched.c's own precedent for "correct, not
 * maximally efficient" tradeoffs at this project's size). Falls back to 0
 * (the BSP) if called before smp_init has populated the table, or if this
 * CPU's id genuinely isn't found - should never happen for any CPU this
 * kernel itself started, but a safe default beats an out-of-bounds index. */
int smp_current_cpu(void);

int smp_is_initialized(void);

/* kernel/sched/sched.c calls this once per real PIT tick (still BSP-only -
 * the 8259 only ever delivers to the BSP) so every *other* online CPU also
 * gets a scheduler-tick opportunity, via IPI_SCHEDULE_VECTOR - this
 * kernel's AP preemption is "broadcast off the one real hardware timer",
 * not per-core APIC timers, a deliberate simplification (see the M-stretch
 * progress log entry for why). No-op before smp_init runs or if no APs
 * ever came online. */
void smp_broadcast_schedule_tick(void);

/* panic.c's SMP-safety hook: broadcasts an NMI to every other online CPU.
 * Each one lands in isr.c's isr_handler for vector 2 (NMI), which - for
 * any vector other than the breakpoint self-test - already does exactly
 * what's wanted here: print and call panic() itself, which halts that
 * core forever. No new fault-handling code needed; just the broadcast. */
void smp_halt_other_cpus(void);
