#pragma once

#include <stdint.h>

#define PMM_DMA_LIMIT 0x100000000ULL

void pmm_init(const uint32_t *e820_map);

uint64_t pmm_alloc_frame(void);

uint64_t pmm_try_alloc_frame(void);

uint64_t pmm_alloc_frame_dma(void);

void pmm_free_frame(uint64_t phys_addr);

uint64_t pmm_free_frame_count(void);

uint64_t pmm_total_frame_count(void);
uint64_t pmm_tracked_limit(void);

uint64_t pmm_alloc_frame_above(uint64_t min_phys);

uint64_t pmm_alloc_contiguous(uint64_t count);

uint64_t pmm_try_alloc_contiguous(uint64_t count);
void pmm_free_contiguous(uint64_t phys_addr, uint64_t count);

void pmm_frame_ref(uint64_t phys_addr);
uint8_t pmm_frame_refs(uint64_t phys_addr);
