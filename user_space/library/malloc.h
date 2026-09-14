#pragma once

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);

int posix_memalign(void **out, size_t alignment, size_t size);
void *aligned_alloc(size_t alignment, size_t size);

size_t malloc_usable_size(void *ptr);
