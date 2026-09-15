#pragma once

#include <stddef.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size) __THROW;
void *calloc(size_t count, size_t size) __THROW;
void *realloc(void *ptr, size_t size) __THROW;
void free(void *ptr) __THROW;

int posix_memalign(void **out, size_t alignment, size_t size) __THROW;
void *aligned_alloc(size_t alignment, size_t size) __THROW;

size_t malloc_usable_size(void *ptr) __THROW;

#ifdef __cplusplus
}
#endif
