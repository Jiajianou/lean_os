#pragma once

#include <stddef.h>

void *k_memcpy(void *destination, const void *source, size_t n);

void *k_memmove(void *destination, const void *source, size_t n);
void *k_memset(void *destination, int c, size_t n);
size_t k_strlen(const char *s);

int k_memcmp(const void *a, const void *b, size_t n);
int k_strcmp(const char *a, const char *b);
void k_strlcpy(char *destination, const char *source, size_t n);

const char *k_strstr(const char *haystack, const char *needle);
