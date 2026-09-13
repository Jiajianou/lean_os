#pragma once

#include <stdint.h>

#define PHYSICAL_MEMORY_DMA_LIMIT 0x100000000ULL

void physical_memory_init(const uint32_t *e820_map);

uint64_t physical_memory_alloc_frame(void);

uint64_t physical_memory_try_alloc_frame(void);

uint64_t physical_memory_alloc_frame_dma(void);

void physical_memory_free_frame(uint64_t phys_address);

uint64_t physical_memory_free_frame_count(void);

uint64_t physical_memory_total_frame_count(void);
uint64_t physical_memory_tracked_limit(void);

uint64_t physical_memory_alloc_frame_above(uint64_t min_phys);

uint64_t physical_memory_alloc_contiguous(uint64_t count);

uint64_t physical_memory_try_alloc_contiguous(uint64_t count);
void physical_memory_free_contiguous(uint64_t phys_address, uint64_t count);

void physical_memory_frame_reference(uint64_t phys_address);
uint8_t physical_memory_frame_refs(uint64_t phys_address);
