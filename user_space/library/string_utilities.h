#pragma once

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t n);
void *memset(void *destination, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
