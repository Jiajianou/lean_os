#pragma once

#include <stdint.h>

void scheduler_paint_diagnostics(void);

void scheduler_diagnostics_toggle(void);

void scheduler_diagnostics_tick(uint64_t ticks);

void scheduler_hang_check(void);
int scheduler_hang_report_requested(int cpu);
void scheduler_hang_report_this_cpu(int cpu, uint64_t rip, uint64_t rsp, int user_mode);
void scheduler_log_kernel_returns(uint64_t from, uint64_t stack_top);

typedef struct {
    uint32_t tickless;
    uint32_t kernel_loops;
    uint32_t lock_waits;
    uint32_t answers;
    uint32_t starved;
    int last_lock_holder;
    uint64_t last_answer_rip;
} hang_statistics_t;

void scheduler_hang_set_thresholds(uint32_t tickless_seconds, uint32_t kernel_seconds,
                                   uint32_t lock_wait_ms);
void scheduler_hang_statistics(hang_statistics_t *out);
int scheduler_hang_lock_was_reported(const void *lock, int holder);
void scheduler_log_task_table(void);
