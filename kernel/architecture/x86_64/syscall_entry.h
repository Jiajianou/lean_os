#pragma once

#include "architecture/x86_64/interrupt_service_routines.h"

void syscall_handler(isr_regs_t *regs);

int signal_deliver_fault(isr_regs_t *regs, int signo, uint64_t fault_address);
int signal_deliver_fault_with_code(isr_regs_t *regs, int signo,
                                   uint64_t fault_address, int fault_code);

/* M203: why SYS_waitfds returned - a descriptor was ready, the pointer
   moved, a program exited, the caller asked not to wait, the deadline came,
   or a signal arrived. Counted since boot. */
#define WAITFDS_RETURN_READY    0
#define WAITFDS_RETURN_POINTER  1
#define WAITFDS_RETURN_EXITS    2
#define WAITFDS_RETURN_ZERO     3
#define WAITFDS_RETURN_DEADLINE 4
#define WAITFDS_RETURN_SIGNAL   5
#define WAITFDS_RETURN_REASONS  6

uint64_t syscall_waitfds_returns(int reason);
