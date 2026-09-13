#include "fpu.h"

#include "library/libk.h"

#define CR0_MP (1u << 1)
#define CR0_EM (1u << 2)
#define CR0_NE (1u << 5)

#define CR4_OSFXSR     (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)

#define FPU_DEFAULT_FCW   0x037Fu
#define FPU_DEFAULT_MXCSR 0x1F80u

void fpu_init_cpu(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(uint64_t)CR0_EM;
    cr0 |= CR0_MP | CR0_NE;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("fninit");
    uint32_t mxcsr = FPU_DEFAULT_MXCSR;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

void fpu_state_init(uint8_t *state) {
    k_memset(state, 0, FPU_STATE_SIZE);
    state[0] = (uint8_t)(FPU_DEFAULT_FCW & 0xFF);
    state[1] = (uint8_t)(FPU_DEFAULT_FCW >> 8);
    state[24] = (uint8_t)(FPU_DEFAULT_MXCSR & 0xFF);
    state[25] = (uint8_t)((FPU_DEFAULT_MXCSR >> 8) & 0xFF);
}

void fpu_save(uint8_t *state) {
    __asm__ volatile("fxsave (%0)" : : "r"(state) : "memory");
}

void fpu_restore(const uint8_t *state) {
    __asm__ volatile("fxrstor (%0)" : : "r"(state) : "memory");
}
