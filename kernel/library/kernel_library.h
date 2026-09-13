#pragma once

#include <stddef.h>

void *k_memcpy(void *dst, const void *src, size_t n);

void *k_memmove(void *dst, const void *src, size_t n);
void *k_memset(void *dst, int c, size_t n);
size_t k_strlen(const char *s);

int k_memcmp(const void *a, const void *b, size_t n);
int k_strcmp(const char *a, const char *b);
void k_strlcpy(char *dst, const char *src, size_t n);

const char *k_strstr(const char *haystack, const char *needle);
