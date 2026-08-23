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

/* Panics if physical memory is exhausted - correct for every call site
 * that has no way to fail cleanly itself (page-table/heap growth, driver
 * init, ELF loading before a task's first instruction runs), which is
 * most of them. Anything reachable from a user-mode syscall on a path
 * that scopes the milestone (SYS_sbrk, shm_create - both M29) must use
 * pmm_try_alloc_frame instead: it's never acceptable for one process
 * running its own address space out of physical memory to take down
 * every other task's along with it. */
uint64_t pmm_alloc_frame(void);

/* Same allocation as pmm_alloc_frame, but returns 0 instead of panicking
 * once physical memory is exhausted - 0 is never a valid frame address
 * (frame 0 sits inside pmm_init's own permanently-reserved low-memory
 * range), so it's a safe, unambiguous "out of memory" sentinel for a
 * caller with a real failure path of its own (M29). */
uint64_t pmm_try_alloc_frame(void);

void pmm_free_frame(uint64_t phys_addr);

/* Allocates `count` physically *contiguous* frames, returning the base
 * address of the run - pmm_alloc_frame alone can't guarantee that, and
 * hardware DMA rings (the RTL8139 NIC driver's receive buffer) are
 * described by a single base address plus a length, not a scatter list.
 * Frees as a block via pmm_free_contiguous, not `count` separate
 * pmm_free_frame calls. */
uint64_t pmm_alloc_contiguous(uint64_t count);
void pmm_free_contiguous(uint64_t phys_addr, uint64_t count);
