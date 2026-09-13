#pragma once

#include <stddef.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

size_t heap_used_bytes(void);
size_t heap_total_bytes(void);
