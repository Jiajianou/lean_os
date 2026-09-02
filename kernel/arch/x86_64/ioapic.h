/* kernel/arch/x86_64/ioapic.h - M103
 *
 * The interrupt controller a machine built after 1996 actually has.
 *
 * ---- what it replaces, and what it does not ---------------------------
 *
 * The 8259 PIC (pic.c) is masked rather than deleted. Two reasons, and
 * the second is the one that matters:
 *
 *   - a machine whose firmware reports no I/O APIC at all is a machine
 *     this kernel still has to boot, and the PIC is what it boots on.
 *     `ioapic_available()` is how every caller asks which world it is
 *     in.
 *   - spurious interrupts. A PIC that is merely masked can still deliver
 *     one on IRQ 7 or 15, and the handler for those vectors has to keep
 *     existing. Deleting the driver would mean an unexpected vector with
 *     nothing behind it.
 *
 * ---- the overrides, which are the whole reason this is not trivial ----
 *
 * A driver knows an ISA IRQ number - the keyboard is 1, the timer is 0.
 * An I/O APIC knows Global System Interrupts. On essentially every PC
 * ever built the timer's IRQ 0 is wired to GSI 2, and the MADT says so
 * with an Interrupt Source Override entry. A kernel that assumed
 * GSI == IRQ would program a line nothing is connected to and get no
 * timer ticks at all - which is the failure this indirection exists to
 * prevent, and it is the normal case rather than an oddity.
 *
 * ---- delivery ---------------------------------------------------------
 *
 * Physical mode, to one CPU's LAPIC, with the vector the PIC path
 * already uses (PIC_IRQ_VECTOR_OFFSET + irq). Keeping the vector numbers
 * identical means kernel/arch/x86_64/isr.c's dispatch table does not
 * care which controller delivered the interrupt, which is what makes
 * "the same self-tests, with the PIC masked" a meaningful thing to run.
 *
 * Lowest-priority delivery to a set of CPUs would spread the load, and
 * is deliberately not done: it needs a policy about which interrupts go
 * where, and M69's rule is that a policy without a measurement is a
 * guess. What IS measured, per vector per CPU, is ioapic_irq_count() -
 * see /proc/interrupts.
 */
#pragma once

#include <stdint.h>

/* Parses what ACPI found and programs every redirection entry to
 * "masked". Safe to call when there is no I/O APIC: it records that and
 * every other call here becomes a no-op, which is what leaves the PIC in
 * charge. */
void ioapic_init(void);

/* 1 if this machine has one and this kernel is using it. */
int ioapic_available(void);

/* Routes `irq` (an ISA IRQ number a driver knows) to the CPU with
 * `lapic_id`, and unmasks it. The override table is applied here, which
 * is why a driver passes the number it knows rather than a GSI. */
void ioapic_route_irq(uint8_t irq, uint8_t lapic_id);

/* Masks it again. */
void ioapic_mask_irq(uint8_t irq);

/* M103: how many interrupts have arrived on each vector, per CPU.
 *
 * The counter exists because "an interrupt that stops arriving is
 * otherwise indistinguishable from a device that has nothing to say" -
 * which is M103's own bullet, and is the difference between a disk that
 * is idle and a disk whose line was routed to a CPU that is not
 * listening. Read out through /proc/interrupts. */
void ioapic_count_irq(uint8_t vector, int cpu);
uint64_t ioapic_irq_count(uint8_t vector, int cpu);
