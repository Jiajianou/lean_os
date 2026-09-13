#pragma once

void keyboard_init(void);

int keyboard_read(void);

int keyboard_peek(void);

void keyboard_inject(char ch, int mods);

int keyboard_modifiers(void);
