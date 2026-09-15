#pragma once

#include "architecture/x86_64/interrupt_service_routines.h"

void syscall_handler(isr_regs_t *regs);

int signal_deliver_fault(isr_regs_t *regs, int signo, uint64_t fault_address);
int signal_deliver_fault_with_code(isr_regs_t *regs, int signo,
                                   uint64_t fault_address, int fault_code);
