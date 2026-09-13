#pragma once

#include <stdint.h>

#define PIT_HZ 100

void pit_init(void);
uint64_t pit_get_ticks(void);

void pit_sleep_ms(uint32_t ms);

void pit_set_tick_hook(void (*hook)(void));
