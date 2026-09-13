#pragma once

#include <stdint.h>

#include "architecture/x86_64/isr.h"
#include "profile.h"

#define PROF_BUCKETS 2048

void profile_reset(void);

void profile_start(void);
void profile_stop(void);

void profile_sample(isr_regs_t *regs);

void profile_get_stats(prof_stats_t *out);

int profile_snapshot(prof_sample_t *out, int max);
