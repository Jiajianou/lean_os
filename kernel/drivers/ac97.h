#pragma once

#include <stdint.h>

#define AC97_SAMPLE_RATE 48000
#define AC97_CHANNELS    2

void ac97_init(void);
int ac97_available(void);

int ac97_play(const int16_t *samples, uint32_t frames);

uint32_t ac97_max_frames(void);

uint32_t ac97_completions(void);

void ac97_set_volume(uint32_t percent);

void ac97_debug_dump(void);
