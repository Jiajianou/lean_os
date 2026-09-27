#pragma once

void panic(const char *message) __attribute__((noreturn));
void panic_with_detail(const char *message, const char *detail) __attribute__((noreturn));

void panic_render(const char *message);
