#pragma once

#include <stdint.h>

#include "power_mode.h"

void power_init(void);

int power_orderly_stop(uint64_t grace_ticks);

void power_shutdown(int mode) __attribute__((noreturn));
