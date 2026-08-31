/* kernel/mm/pmm.h
 *
 * Physical frame allocator: a bitmap over every frame of physical memory
 * the boot loader's e820-format map describes (see e820.h). One bit per
 * 4 KiB frame; 1 = reserved/used, 0 = free.
 *
 * M90: this used to be a fixed bitmap over a hardcoded 1 GiB, and the
 * header said why - "a frame this allocator hands out has to be
 * addressable before any virtual-memory mapping exists for it (page
 * tables themselves are built out of pmm-allocated frames), so it can
 * never track more than what's already mapped 1:1. Extending past 1 GiB
 * is future work for whoever needs more than that much physical memory
 * tracked."
 *
 * The constraint was exactly right and has not gone away; what changed is
 * that vmm_init now builds its identity map from the same e820 map this
 * file is seeded from, so "what is mapped 1:1" is "all of RAM" rather
 * than a constant. The ordering that makes that work is the awkward part
 * and it lives in pmm_init: the bitmap has to be stored somewhere, and
 * the only memory it may be stored in is memory it has not described
 * yet. See pmm_init's own comment for how that knot is untied.
 *
 * A second thing the fixed 1 GiB was quietly providing: every frame was
 * below 4 GiB, so a 32-bit DMA engine could be handed any of them. That
 * is no longer true, and pmm_alloc_frame_dma/pmm_alloc_contiguous exist
 * to say so at the call site rather than to be discovered by a NIC
 * writing packets into the wrong half of memory.
 */
#pragma once

#include <stdint.h>

/* The ceiling a 32-bit DMA engine can address. rtl8139.c hands the card a
 * 32-bit physical base; ac97.c writes a 32-bit buffer-descriptor address.
 * Neither can be given a frame above this, and neither could ever have
 * been handed one before M90 - the allocator only tracked the first
 * gigabyte. */
#define PMM_DMA_LIMIT 0x100000000ULL /* 4 GiB */

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

/* M90: a single frame below PMM_DMA_LIMIT, for a caller that will hand
 * the address to a device that can only express 32 bits of it. Panics on
 * exhaustion like pmm_alloc_frame, because every caller is a driver
 * initialising at boot with no failure path of its own. */
uint64_t pmm_alloc_frame_dma(void);

void pmm_free_frame(uint64_t phys_addr);

/* M40: how many 4 KiB frames are currently free. Exists for the boot
 * self-tests: comparing this across an operation that is supposed to fail
 * is what turns "the syscall returned -1" into "the syscall returned -1
 * and didn't leak anything doing it", which is the actually interesting
 * half of a failure-path audit. */
uint64_t pmm_free_frame_count(void);

/* M90: how many frames this allocator describes at all, and the physical
 * address one past the last of them. Both were compile-time constants
 * until the map started coming from firmware; the self-tests that want to
 * say "this machine has more than a gigabyte and the allocator knows it"
 * need to be able to ask. */
uint64_t pmm_total_frame_count(void);
uint64_t pmm_tracked_limit(void);

/* M90: a frame that is currently free and lies at or above `min_phys`,
 * allocated. 0 if there is none. The only caller is the boot self-test
 * that has to prove a frame above the old 1 GiB ceiling is both
 * allocatable and addressable - "the allocator reports a big number" is
 * not that proof, and neither is an ordinary allocation, which comes off
 * the bottom of the bitmap and would pass identically on a machine with
 * 128 MiB. */
uint64_t pmm_alloc_frame_above(uint64_t min_phys);

/* Allocates `count` physically *contiguous* frames, returning the base
 * address of the run - pmm_alloc_frame alone can't guarantee that, and
 * hardware DMA rings (the RTL8139 NIC driver's receive buffer) are
 * described by a single base address plus a length, not a scatter list.
 * Frees as a block via pmm_free_contiguous, not `count` separate
 * pmm_free_frame calls.
 *
 * M90: the run always comes from below PMM_DMA_LIMIT. Both kinds of
 * caller are fine with that - a DMA ring must be, and a task's kernel
 * stack does not care - and one rule that is always safe beats two rules
 * where the unsafe one is the default. */
uint64_t pmm_alloc_contiguous(uint64_t count);
void pmm_free_contiguous(uint64_t phys_addr, uint64_t count);

/* ---- M82: frame ownership --------------------------------------------
 *
 * A frame used to have exactly one owner, so the allocator's "is this
 * frame free" bit answered both questions at once. Copy-on-write breaks
 * that - see pmm.c's frame_refs for the whole argument. pmm_frame_ref
 * adds an owner to an already-allocated frame; pmm_free_frame removes
 * one and only returns the frame to the free list when the last owner
 * lets go, which is what every existing single-owner caller was already
 * doing without knowing it. */
void pmm_frame_ref(uint64_t phys_addr);
uint8_t pmm_frame_refs(uint64_t phys_addr);
