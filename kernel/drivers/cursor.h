/* kernel/drivers/cursor.h
 *
 * Mouse cursor sprite drawn directly over the framebuffer - save the
 * pixels underneath before drawing, restore them before moving. Good
 * enough for M18's bring-up (nothing else owns the framebuffer yet), but
 * this is exactly the kind of thing a compositor (M20) takes over for
 * real: once other windows can also change what's under the cursor
 * without going through this code, "restore what I saved" stops being
 * correct.
 */
#pragma once

#include <stdint.h>

void cursor_init(int32_t x, int32_t y);
void cursor_move(int32_t dx, int32_t dy);
int32_t cursor_x(void);
int32_t cursor_y(void);
