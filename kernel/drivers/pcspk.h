#pragma once

#include <stdint.h>

void pcspk_init(void);

void pcspk_tone(uint32_t freq_hz, uint32_t ms);

void pcspk_off(void);

void pcspk_set_muted(int muted);

void pcspk_tick(void);
