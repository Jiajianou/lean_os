#pragma once

void panic(const char *message) __attribute__((noreturn));

void panic_render(const char *message);
