#pragma once

#include <stdint.h>

void scheduler_paint_diagnostics(void);

void scheduler_diagnostics_toggle(void);

void scheduler_diagnostics_tick(uint64_t ticks);
