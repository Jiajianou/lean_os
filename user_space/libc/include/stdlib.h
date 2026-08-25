/* user_space/libc/include/stdlib.h - M63. See string.h's header comment
 * for why this exists and what "ours, not somebody else's" means.
 *
 * malloc and free still come from user_space/lib/malloc.c, which has
 * been most of one piece of a libc since M19. */
#pragma once

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
double strtod(const char *s, char **end);

int abs(int v);
long labs(long v);

#define RAND_MAX 32767
int rand(void);
void srand(unsigned int seed);
