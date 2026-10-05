#pragma once

#include <stdint.h>

void console_init(void);
void console_putc(char c);
void console_puts(const char *s);

/* M225. A scroll no longer repaints the screen inside console_putc: it moves
   the character grid (memory) and leaves the framebuffer to be repainted
   from it a few cells at a time, by whoever feeds the console - the log's
   drain, between interrupts-off sections. Until that is done the rows not
   yet repainted show what was there before the scroll, and characters
   written to them go to the grid and reach the screen with the repaint.

   console_repaint_pending: whether a repaint is owed.
   console_repaint_step: repaints the next piece, in screen order - one
   glyph, or a run of at most `max_cells` blank cells in one fill - and
   returns how many cells it was (0 when none is owed).
   console_cells_drawn: how many cells the console has painted since boot -
   a glyph or a blank is one; the old full-screen scroll of a screen too big
   for the grid counts as every cell of it. */
int console_repaint_pending(void);
uint32_t console_repaint_step(uint32_t max_cells);
uint64_t console_cells_drawn(void);

/* 0 for a screen of more cells than the console keeps a grid for: it
   scrolls the old way, by copying the framebuffer up inside the character
   (every cell of the screen in one section). */
int console_has_grid(void);
