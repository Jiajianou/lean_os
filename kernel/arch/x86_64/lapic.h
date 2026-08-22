/* kernel/arch/x86_64/lapic.h
 *
 * Local APIC driver. Distinct from kernel/arch/x86_64/pic.h's 8259: the
 * PIC (still in use, unmodified by SMP) delivers legacy device IRQs
 * (PIT, keyboard, mouse, ATA) to the BSP the same way it always has -
 * nothing here changes that. The Local APIC is a separate, per-CPU piece
 * of hardware every core has one of, and it's the only thing that can (a)
 * start another core running at all (INIT-SIPI-SIPI, see smp.c) and (b)
 * deliver an inter-processor interrupt (IPI) once more than one core is
 * running (smp.c's scheduler-tick broadcast, panic.c's halt-everyone-else
 * broadcast).
 */
#pragma once

#include <stdint.h>

#define LAPIC_ICR_DELIVERY_FIXED   0x00000u
#define LAPIC_ICR_DELIVERY_NMI     0x00400u
#define LAPIC_ICR_DELIVERY_INIT    0x00500u
#define LAPIC_ICR_DELIVERY_STARTUP 0x00600u
#define LAPIC_ICR_LEVEL_ASSERT     0x04000u
#define LAPIC_ICR_DEST_ALL_EXCL_SELF 0xC0000u

/* Maps the given physical MMIO base (from the MADT, kernel/acpi/acpi.h)
 * virt==phys into the kernel's address space and enables this CPU's Local
 * APIC (Spurious Interrupt Vector Register, software-enable bit). Called
 * once by the BSP (with the real MMIO base) and once more by every AP
 * (lapic_init_this_cpu, reusing the base the BSP already mapped - the
 * mapping itself lives in the shared kernel PML4[0], so only the *enable*
 * step needs repeating per core, not the vmm_map_page). */
void lapic_init(uint64_t phys_base);
void lapic_init_this_cpu(void);

uint32_t lapic_id(void);
void lapic_send_eoi(void);

/* Blocks (polling the ICR's delivery-status bit) until any IPI already in
 * flight has been accepted, then sends this one and waits for it to be
 * accepted too - the standard "don't queue a second IPI while the first
 * is still being delivered" discipline every Local APIC needs. */
void lapic_send_ipi(uint32_t apic_id, uint32_t vector_and_flags);
void lapic_send_ipi_all_excl_self(uint32_t vector_and_flags);
