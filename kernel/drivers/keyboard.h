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

/* Which of Ctrl/Alt/Shift were held down for the character keyboard_read
 * most recently returned, as a bitmask of system_api/include/input.h's
 * KBD_MOD_* bits. Added at M32 so a caller could tell a plain 'c'
 * keypress from a Ctrl+C one (compositor.c's Alt+Tab, gui_terminal.c's
 * Ctrl+C/V) - the buffered ASCII stream alone can't.
 *
 * M40: this used to report *live* held/not-held state, sampled whenever
 * the caller happened to ask. Every caller asks immediately after
 * keyboard_read, but "immediately" is a full compositor frame away from
 * when the key was actually pressed, and by then the modifier can be
 * released - so Alt+Tab silently did nothing whenever the chord was
 * shorter than one redraw. (Reproduced by M40's input harness with a
 * 100ms `sendkey alt-tab`; a human holding Alt for a quarter second
 * never noticed.) The modifier bits are now captured in the IRQ handler
 * alongside each character and travel with it through the ring buffer,
 * so a chord can't come apart in transit no matter how long the reader
 * takes to get to it. Left Ctrl/Alt only, same "no 0xE0-prefixed
 * extended scancodes yet" limitation the rest of this driver has. */
int keyboard_modifiers(void);
