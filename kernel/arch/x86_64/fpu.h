/* kernel/arch/x86_64/fpu.h
 *
 * M63: x87 and SSE state, which did not exist in this kernel at all.
 *
 * Both CFLAGS and USER_CFLAGS carried `-mgeneral-regs-only` from M20
 * onward, and that was a correct and deliberate simplification - a kernel
 * that never touches SSE is a kernel that never has to save it. It was
 * also an absolute wall: essentially no real C program compiles without
 * `double`, so "run a program nobody in this repo wrote" starts here.
 *
 * What changes, and what deliberately does not:
 *
 *   - `-mgeneral-regs-only` is dropped from USER_CFLAGS **only**. The
 *     kernel keeps it, which is the whole reason this is cheap: kernel
 *     code cannot touch xmm registers, so an interrupt does not have to
 *     save them and neither does a syscall. Only a *task switch* does.
 *   - CR0.EM is cleared and CR0.MP/NE set, so SSE instructions execute
 *     rather than trapping; CR4.OSFXSR says "this OS uses FXSAVE" and
 *     CR4.OSXMMEXCPT routes SIMD exceptions to #XF rather than #UD.
 *     Done per CPU, because control registers are per CPU.
 *   - State is saved and restored around context_switch with FXSAVE /
 *     FXRSTOR: 512 bytes per task, and the switch loads the *incoming*
 *     task's state before switching rather than restoring on the way
 *     back. That ordering is what gives a brand-new task a clean FPU
 *     instead of whatever the last task left in the registers - a fresh
 *     task never returns from context_switch, so there is no "on the way
 *     back" for it.
 */
#pragma once

#include <stdint.h>

/* The FXSAVE area: 512 bytes, 16-byte aligned, and both are architectural
 * requirements rather than preferences. */
#define FPU_STATE_SIZE  512
#define FPU_STATE_ALIGN 16

/* Enables SSE on the calling CPU. Called once by the BSP and once by each
 * AP as it comes up. */
void fpu_init_cpu(void);

/* Fills `state` with the FPU/SSE state a freshly created task should
 * start with - the x87 control word and MXCSR at their architectural
 * defaults, every register zero. Not simply a zeroed buffer: an all-zero
 * MXCSR unmasks every SIMD exception, so the first denormal a program
 * produced would fault. */
void fpu_state_init(uint8_t *state);

void fpu_save(uint8_t *state);
void fpu_restore(const uint8_t *state);
