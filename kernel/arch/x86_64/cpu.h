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

/* ---- M96: the two model-specific registers this kernel writes by name -
 *
 * IA32_FS_BASE is the thread pointer. x86-64 has no way to load a
 * segment base for %fs from a descriptor in long mode - the GDT entry's
 * base is ignored - so the only way to make `%fs:0` mean something
 * per-thread is to write this MSR on every switch. That is what makes
 * `__thread` and errno-per-thread possible at all.
 *
 * IA32_GS_BASE is not used and is named here so the next reader knows it
 * was considered: it is what a kernel uses for its own per-CPU data
 * through `swapgs`, and this kernel finds its per-CPU state through
 * smp_current_cpu() instead. Two mechanisms for one thing is what M69's
 * note about the compositor calls a way to be wrong twice.
 */
#define MSR_FS_BASE 0xC0000100u
#define MSR_GS_BASE 0xC0000101u

static inline void cpu_write_msr(unsigned int msr, unsigned long long value) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((unsigned int)(value & 0xffffffffu)),
                       "d"((unsigned int)(value >> 32)));
}

static inline unsigned long long cpu_read_msr(unsigned int msr) {
    unsigned int lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((unsigned long long)hi << 32) | lo;
}
