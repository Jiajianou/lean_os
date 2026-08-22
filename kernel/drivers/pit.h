/* kernel/drivers/pit.h
 *
 * Intel 8253/8254 Programmable Interval Timer, channel 0, wired to IRQ0.
 * Programmed for a fixed PIT_HZ tick rate; pit_init() registers an IRQ0
 * handler (isr.h's irq_register_handler) and unmasks the line, so this is
 * the point interrupts start actually firing - see kernel.c for where
 * `sti` first runs.
 */
#pragma once

#include <stdint.h>

#define PIT_HZ 100 /* 10 ms per tick - coarse enough to be cheap, fine enough for M7's scheduler */

void pit_init(void);
uint64_t pit_get_ticks(void);

/* Busy-waits (via `hlt` between ticks, so the CPU actually idles) until at
 * least ms milliseconds have elapsed. Rounds up to the nearest tick. */
void pit_sleep_ms(uint32_t ms);

/* Registers a function to be called at the end of every tick's IRQ0
 * handler (still inside interrupt context). One slot - the scheduler
 * (kernel/sched/sched.c) is the only thing that needs this right now. */
void pit_set_tick_hook(void (*hook)(void));
