#pragma once

#define MAX_CPUS 16

#define MSR_FS_BASE 0xC0000100u
#define MSR_GS_BASE 0xC0000101u

#if defined(__x86_64__) && !defined(LEANOS_HOST_TEST)
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

#if defined(__x86_64__) && !defined(LEANOS_HOST_TEST)
static inline void cpu_enable_interrupts(void) { __asm__ volatile("sti"); }
static inline void cpu_disable_interrupts(void) { __asm__ volatile("cli"); }
static inline void cpu_spin_hint(void) { __asm__ volatile("pause"); }
static inline unsigned long long cpu_stack_pointer(void) {
    unsigned long long sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    return sp;
}
#else
void cpu_enable_interrupts(void);
void cpu_disable_interrupts(void);
void cpu_spin_hint(void);
unsigned long long cpu_stack_pointer(void);
#endif
