#pragma once

#include <stdint.h>

#include "display.h"

void dispi_init(void);

int dispi_available(void);


int dispi_get_modes(display_mode_t *out, int max);

int dispi_set_mode(uint32_t w, uint32_t h, uint32_t *out_pitch);
