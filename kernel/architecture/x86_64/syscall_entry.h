#pragma once

#include "architecture/x86_64/interrupt_service_routines.h"

void syscall_handler(isr_regs_t *regs);

int signal_deliver_fault(isr_regs_t *regs, int signo, uint64_t fault_address);
