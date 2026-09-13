#pragma once

#include <stdint.h>

#define FPU_STATE_SIZE  512
#define FPU_STATE_ALIGN 16

void fpu_init_cpu(void);

void fpu_state_init(uint8_t *state);

void fpu_save(uint8_t *state);
void fpu_restore(const uint8_t *state);
