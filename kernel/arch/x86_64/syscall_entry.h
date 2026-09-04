/* kernel/arch/x86_64/syscall_entry.h
 *
 * Kernel-side entry point for the `int 0x80` syscall gate (idt.c installs
 * it at vector 0x80, isr_asm.asm's syscall_common_stub calls this).
 * Syscall numbering/ABI itself lives in system_api/include/syscall.h -
 * the contract shared with user_space. Named differently from that
 * header (rather than the more obvious "syscall.h") specifically so a
 * quoted `#include "syscall.h"` from syscall.c unambiguously reaches the
 * shared one instead of resolving to this same-directory file first.
 */
#pragma once

#include "arch/x86_64/isr.h"

void syscall_handler(isr_regs_t *regs);

/* M99: deliver a synchronous fault to a handler the faulting program
 * installed, if it installed one.
 *
 * Lives here because the frame-building code lives in syscall.c and
 * there is exactly one of it: a signal frame is a signal frame whether
 * the signal came from `kill` or from the memory-management unit, and
 * two of them would be two chances to get the alignment wrong.
 *
 * Returns 1 when `regs` has been rewritten to enter the handler - the
 * caller returns from the interrupt and the program resumes there. It
 * returns 0 when there is no handler, when the signal is blocked
 * (which is the case INSIDE its own handler, and is what stops a
 * faulting handler from looping), or when the frame could not be written
 * to the program's stack (which is the case when the stack is what
 * faulted). A 0 means the caller should do what it did before M99:
 * terminate the task. */
int signal_deliver_fault(isr_regs_t *regs, int signo, uint64_t fault_addr);
