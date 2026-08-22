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
 * supports). No permission model, no refcounting, no destroy - not
 * needed yet for what this project ships.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Allocates `size` bytes (rounded up to whole pages) of physical memory
 * and registers it under a new id. Returns the id (>= 0), or -1 if the
 * segment table is full or a frame allocation fails partway through. */
int shm_create(size_t size);

/* Size in bytes of segment `id` (as originally requested, not rounded up
 * to pages), or -1 if `id` doesn't name a live segment. */
int64_t shm_get_size(int id);

/* Maps every page of segment `id` into the given address space starting
 * at `vaddr` (page-aligned), with the given vmm flags (VMM_FLAG_WRITABLE/
 * VMM_FLAG_USER - see mm/vmm.h). Returns 0 on success, -1 if `id` is
 * invalid. */
int shm_map_into(int id, uint64_t pml4_phys, uint64_t vaddr, uint64_t flags);
