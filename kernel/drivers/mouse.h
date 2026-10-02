#pragma once

#include <stdint.h>

#include "input.h"

void mouse_init(void);

int mouse_is_present(void);

int mouse_read(mouse_event_t *ev);
int mouse_pending(void);

void mouse_inject(int32_t dx, int32_t dy, uint8_t buttons, int32_t wheel);

void mouse_inject_from(uint8_t source, uint8_t flags, int32_t dx, int32_t dy, uint8_t buttons, int32_t wheel);

int mouse_labelled_trackpad(void);

int mouse_wheel_present(void);
