/* kernel/ipc/shm.h
 *
 * M19: kernel-owned shared memory segments. A segment is a fixed set of
 * physical frames, allocated once (shm_create) and independent of any
 * process's address space; any process that knows its id can map it in
 * (shm_map_into) at a virtual address of its own choosing. Two processes
 * mapping the same id see the same physical pages - real sharing, not a
 * copy - which is the whole reason this exists instead of just using a
 * pipe (kernel/ipc/pipe.h): pipes copy data through a small fixed buffer,
 * fine for a shell's byte stream but far too slow for a compositor and
 * an app trading whole video frames (M20+).
 *
 * Ids are a flat global namespace (not scoped to the creating process) -
 * simplest thing that lets an unrelated process attach to a segment it
 * learns the id of some other way (M19's own self-test passes one
 * through the same single-string argv mechanism SYS_spawn already
 * supports). No permission model, no refcounting - not needed yet for
 * what this project ships.
 *
 * Every segment does still remember which task created it, though (see
 * `owner_task_id` below), because MAX_SHM_SEGMENTS is small and fixed:
 * without a way to reclaim a dead creator's segments, every window this
 * OS ever composites (each backed by its own segment, created by the
 * compositor - see user_space/bin/compositor.c's accept_pending_window/
 * back_buf) would permanently consume a slot, and a handful of processes
 * opening and closing windows would eventually exhaust the table for the
 * whole system, forever, with every further window silently failing to
 * open - not a hypothetical, this is exactly what the kernel's own boot
 * self-tests (kernel.c) do every single boot, each spawning a throwaway
 * compositor + client(s) and then SIGKILLing them.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Allocates `size` bytes (rounded up to whole pages) of physical memory
 * and registers it under a new id. Returns the id (>= 0), or -1 if the
 * segment table is full or a frame allocation fails partway through.
 * `owner_task_id` is only ever used later, by shm_free_by_owner - it's
 * not a permission check, just bookkeeping for reclaiming the segment
 * once its owner is gone. */
int shm_create(size_t size, int owner_task_id);

/* Frees every live segment owned by `owner_task_id` - called once from
 * task_exit_with_code (sched.c), the one funnel every task-exit path
 * (SYS_exit, SIGKILL/SIGTERM delivery, a normal `return` from main via
 * crt0) already goes through, so a task's shm segments are reclaimed
 * exactly once, right as it's removed from the scheduler. Safe to call
 * for a task that owns none - just a no-op scan. */
void shm_free_by_owner(int owner_task_id);

/* M50: releases segment `id`, but only if `owner_task_id` really created
 * it - the ownership invariant this milestone is about, stated as code
 * rather than as a convention. Returns 0, or -1 for an unknown id or one
 * owned by somebody else.
 *
 * Frees the frames and the table slot and nothing else: it does NOT
 * unmap the segment from any address space, because it has no idea which
 * ones it is mapped into (nothing here tracks that). Unmapping is the
 * caller's half of the contract, and sys_shm_free is where the two halves
 * are put together - see its own comment for why that is the honest
 * split rather than a mapping registry nothing else would use. */
int shm_free(int id, int owner_task_id);

/* Pages segment `id` occupies (its size rounded up), or -1 for an
 * unknown id - what a caller needs to unmap the right range before
 * freeing it. */
int64_t shm_page_count(int id);

/* M45: how many live segments `owner_task_id` currently holds - the shm
 * half of what SYS_taskinfo reports per task, and the number that makes a
 * segment leak visible from user space at all (MAX_SHM_SEGMENTS is 32 and
 * nothing but shm_free_by_owner ever gives one back). Counting rather
 * than listing: a task manager wants "how many", and an id list would be
 * a second, wider ABI for no extra answer. */
int shm_count_by_owner(int owner_task_id);

/* Size in bytes of segment `id` (as originally requested, not rounded up
 * to pages), or -1 if `id` doesn't name a live segment. */
int64_t shm_get_size(int id);

/* Maps every page of segment `id` into the given address space starting
 * at `vaddr` (page-aligned), with the given vmm flags (VMM_FLAG_WRITABLE/
 * VMM_FLAG_USER - see mm/vmm.h). Returns 0 on success, -1 if `id` is
 * invalid. */
int shm_map_into(int id, uint64_t pml4_phys, uint64_t vaddr, uint64_t flags);
