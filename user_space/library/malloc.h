#pragma once

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);

size_t malloc_usable_size(void *ptr);
