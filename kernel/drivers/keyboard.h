/* kernel/drivers/keyboard.h
 *
 * PS/2 keyboard driver, IRQ1, scancode set 1 (the PS/2 controller's
 * legacy default - nothing here reprograms the scancode set). Translates
 * make codes for the main US QWERTY block (letters, digits, punctuation,
 * space/tab/enter/backspace, shift) to ASCII and buffers them; extended
 * (0xE0-prefixed) keys and F-keys/keypad are not decoded yet - not needed
 * until something (M13's shell) actually wants arrow keys etc.
 */
#pragma once

void keyboard_init(void);

/* Returns the next buffered ASCII character, or -1 if none is available.
 * Never blocks. */
int keyboard_read(void);
