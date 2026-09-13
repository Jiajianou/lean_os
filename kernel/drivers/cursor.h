#pragma once

#include <stdint.h>

void cursor_init(int32_t x, int32_t y);
void cursor_move(int32_t dx, int32_t dy);
int32_t cursor_x(void);
int32_t cursor_y(void);
