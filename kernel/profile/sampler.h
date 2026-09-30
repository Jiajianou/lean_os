#pragma once

#include <stdint.h>

#include "architecture/x86_64/interrupt_service_routines.h"
#include "profile.h"
#include "syscall.h"

#define PROF_BUCKETS 2048

void profile_reset(void);

void profile_start(void);
void profile_stop(void);

void profile_sample(isr_regs_t *regs);

void profile_get_statistics(prof_statistics_t *out);

/* M202: index 0 is the kernel working for nobody in particular (an
   interrupt, a kernel task), 1..SYSCALL_COUNT a system call plus one, and
   PROFILE_ACTIVITY_PAGE_FAULT a fault being filled. */
#define PROFILE_ACTIVITY_PAGE_FAULT (SYSCALL_COUNT + 1)
#define PROFILE_ACTIVITIES          (SYSCALL_COUNT + 2)

void profile_set_kernel_only(int on);
uint64_t profile_activity_samples(int activity);

int profile_snapshot(prof_sample_t *out, int max);
