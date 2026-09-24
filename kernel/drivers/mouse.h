#pragma once

#include <stdint.h>

#include "input.h"

void mouse_init(void);

int mouse_is_present(void);

int mouse_read(mouse_event_t *ev);

void mouse_inject(int32_t dx, int32_t dy, uint8_t buttons, int32_t wheel);
