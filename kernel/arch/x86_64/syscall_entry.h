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
