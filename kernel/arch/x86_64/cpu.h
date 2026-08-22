/* kernel/arch/x86_64/cpu.h
 *
 * A single shared constant every SMP-aware file needs: the fixed ceiling
 * on how many logical CPUs this kernel will ever track (GDT TSS slots,
 * per-CPU scheduler state, the ACPI MADT parse result). Split into its own
 * header so gdt.h/sched.h/acpi.h/smp.h can all agree on it without any of
 * them having to depend on smp.h itself just for one number.
 */
#pragma once

#define MAX_CPUS 8
