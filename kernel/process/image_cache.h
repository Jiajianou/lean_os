#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct image_cache_entry image_cache_entry_t;

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t refusals;
    uint64_t evictions;
    uint64_t cached_pages;
} image_cache_statistics_t;

image_cache_entry_t *image_cache_acquire(const char *path);
void image_cache_release(image_cache_entry_t *entry);
const uint8_t *image_cache_header(const image_cache_entry_t *entry, size_t *file_size);
uint64_t image_cache_map(uint64_t pml4_phys, const image_cache_entry_t *entry);
void image_cache_statistics(image_cache_statistics_t *out);
uint64_t image_cache_private_pages(void);
