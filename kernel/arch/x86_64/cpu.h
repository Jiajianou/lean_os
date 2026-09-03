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

/* ---- Q13: why these two are behind an #if -----------------------------
 *
 * `wrmsr` and `rdmsr` are x86 instructions and this header is only ever
 * compiled for x86-64 by the build that ships. It is also compiled by
 * the host test tier, on whatever machine somebody is sitting at - an
 * arm64 Mac, in this project's case - and there these two lines do not
 * fail to link, they fail to *assemble*. No fake .c file can reach an
 * instruction the assembler rejects.
 *
 * So the declaration is the same on both sides and only the definition
 * moves: inline asm for the kernel, a symbol for the host, supplied by
 * tests/fakes/fake_cpu.c. The kernel build takes the first branch
 * unconditionally and is byte-identical to what it was.
 *
 * This is the same trade tests/fakes/arch/x86_64/io.h makes and one step
 * better: a shadowed header replaces the whole file and can drift from
 * it, and this cannot - MAX_CPUS and the MSR numbers above have exactly
 * one definition, which is the half that is load-bearing for every
 * per-CPU array in kernel/sched/sched.c. */
#if defined(__x86_64__)
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
#else
void cpu_write_msr(unsigned int msr, unsigned long long value);
unsigned long long cpu_read_msr(unsigned int msr);
#endif

/* ---- Q13: the two instructions the scheduler used to spell inline -----
 *
 * `sti` and a read of `%rsp` were written as `__asm__ volatile` in the
 * middle of kernel/sched/sched.c, which is 2,157 lines of scheduling
 * policy with two instructions in it. Naming them here is not a wrapper
 * for its own sake: it is what lets that file be compiled for a host and
 * tested, and the names say what the instructions are FOR, which the
 * mnemonics do not. (`hlt` was already named - cpu_halt in io.h, moved
 * there by Q4 for exactly this reason and from exactly this cause.)
 *
 * cpu_stack_pointer is the odd one. It exists for M106's check that this
 * CPU is standing on the stack of the task it thinks it is running, and
 * on a host that question has no meaning - one process, one stack, and
 * no task ever actually switched onto it. So the host answer is one a
 * test supplies rather than a plausible number: see
 * tests/fakes/fake_cpu.c. */
#if defined(__x86_64__)
static inline void cpu_enable_interrupts(void) { __asm__ volatile("sti"); }
/* A hint to the core that this is a spin, not work. The third of the
 * three, and the one kernel/lib/spinlock.h has spelled inline since M7 -
 * this is a second copy only because that header is shadowed for the
 * host tier and this one is not. */
static inline void cpu_spin_hint(void) { __asm__ volatile("pause"); }
static inline unsigned long long cpu_stack_pointer(void) {
    unsigned long long sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    return sp;
}
#else
void cpu_enable_interrupts(void);
void cpu_spin_hint(void);
unsigned long long cpu_stack_pointer(void);
#endif
