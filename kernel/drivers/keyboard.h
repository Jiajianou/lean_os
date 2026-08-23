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

/* M32: live (not buffered) held/not-held state of Ctrl/Alt, as a bitmask
 * of system_api/include/input.h's KBD_MOD_* bits - the modifier-key
 * counterpart to shift's existing (internal-only) tracking, now needed
 * by SYS_kbd_modifiers so a caller can tell a plain 'c' keypress from a
 * Ctrl+C one (compositor.c's Alt+Tab, gui_terminal.c's Ctrl+C/V) since
 * the buffered ASCII stream alone can't. Left Ctrl/Alt only, same
 * "no 0xE0-prefixed extended scancodes yet" limitation the rest of this
 * driver already has (see this file's own header comment). */
int keyboard_modifiers(void);
