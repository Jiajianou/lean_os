#pragma once

#include <stdint.h>

void pc_speaker_init(void);

void pc_speaker_tone(uint32_t freq_hz, uint32_t ms);

void pc_speaker_off(void);

void pc_speaker_set_muted(int muted);

void pc_speaker_tick(void);
