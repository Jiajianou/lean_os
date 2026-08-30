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

/* M68: is there a keystroke buffered, without consuming it. SYS_waitfds
 * has to ask "would a read block" about fd 0 without taking the byte away
 * from whoever actually reads next - which is the whole difference
 * between a poll and a read, and the reason this could not just call
 * keyboard_read and push the answer back. */
int keyboard_peek(void);

/* M56: pushes a synthetic character onto the same ring the IRQ handler
 * feeds, so a reader (SYS_kbd_read, and therefore the compositor) cannot
 * tell it from a real keypress. The counterpart to M51's mouse_inject and
 * it exists for the same reason: a boot self-test that has to drive a
 * *program* - here gui_terminal, which only produces the output whose
 * scrollback [m56] is about if somebody types a command into it.
 *
 * Deliberately not a syscall, exactly as mouse_inject is not: a user
 * program able to forge keystrokes could type into any other program's
 * window.
 *
 * `mods` is the KBD_MOD_* mask (system_api/include/input.h) this
 * character is to be reported with, rather than whatever is physically
 * held - which for an injected key is nothing. Without it a self-test
 * could type letters but never a *chord*, and every interesting thing a
 * client binds is a chord: Ctrl+V, Ctrl+Z, Ctrl+S. */
void keyboard_inject(char ch, int mods);

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
