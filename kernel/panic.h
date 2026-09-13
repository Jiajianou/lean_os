#pragma once

void panic(const char *msg) __attribute__((noreturn));

void panic_render(const char *msg);
