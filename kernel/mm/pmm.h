/* kernel/mm/pmm.h
 *
 * Physical frame allocator: a bitmap over PMM_TRACKED_MEMORY bytes of
 * physical memory, seeded from the e820-format memory map the boot loader
 * hands off (see e820.h). One bit per 4 KiB frame; 1 = reserved/used, 0 =
 * free.
 *
 * PMM_TRACKED_MEMORY is capped at the 1 GiB vmm_init() identity-maps
 * (vmm.h/vmm.c) - a frame this allocator hands out has to be addressable
 * before any virtual-memory mapping exists for it (page tables themselves
 * are built out of pmm-allocated frames), so it can never track more than
 * what's already mapped 1:1. Extending past 1 GiB is future work for
 * whoever needs more than that much physical memory tracked.
 */
#pragma once

#include <stdint.h>

#define PMM_TRACKED_MEMORY 0x40000000ULL /* 1 GiB */

void pmm_init(const uint32_t *e820_map);
uint64_t pmm_alloc_frame(void);
void pmm_free_frame(uint64_t phys_addr);

/* Allocates `count` physically *contiguous* frames, returning the base
 * address of the run - pmm_alloc_frame alone can't guarantee that, and
 * hardware DMA rings (the RTL8139 NIC driver's receive buffer) are
 * described by a single base address plus a length, not a scatter list.
 * Frees as a block via pmm_free_contiguous, not `count` separate
 * pmm_free_frame calls. */
uint64_t pmm_alloc_contiguous(uint64_t count);
void pmm_free_contiguous(uint64_t phys_addr, uint64_t count);
