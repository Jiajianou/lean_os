#include "shm.h"

#include "lib/libk.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/spinlock.h"

#define PAGE_SIZE 4096ULL
/* M41: 16 -> 32. One segment per window plus the compositor's own back
 * buffer, and a segment is deliberately leaked (not freed) whenever a
 * window slot is reused - see user_space/bin/compositor.c's
 * reclaim_window. With MAX_WINDOWS now 12, 16 was less than two full
 * desktops' worth, i.e. an exact-fit cap of exactly the kind M40 found
 * silently breaking the desktop. */
/* M50: 32 is now a real bound rather than a countdown. Until this
 * milestone nothing ever released a segment except a task exiting, so
 * every window this OS composited consumed a slot permanently - about
 * thirty window opens exhausted the table for the life of the machine.
 * With SYS_shm_free, a compositor's live usage is exactly
 * 1 (its back buffer) + one per live window = 13 at WM_MAX_ROUTABLE_WINDOWS,
 * which the boot self-test measures and logs ("[m50] compositor after the
 * storm"). Left at 32 deliberately: the number was never the problem. */
#define MAX_SHM_SEGMENTS 32

typedef struct {
    int used;
    int owner_task_id;
    uint64_t size;       /* requested size in bytes, not page-rounded */
    uint64_t page_count;
    uint64_t *frames;    /* kmalloc'd array of page_count physical addresses */
} shm_segment_t;

static shm_segment_t segments[MAX_SHM_SEGMENTS];

/* ---- M67: shm_lock ---------------------------------------------------
 *
 * What it protects: the `segments` table above - the `used` flag that
 * makes find_free_slot's answer meaningful, and the frame array behind
 * each live entry.
 *
 * Against whom: two tasks calling SYS_shm_create at the same moment.
 * Before M67 that could not happen, because IF was clear for the whole
 * of `int 0x80`; find_free_slot could return a slot and the caller could
 * fill it in with no possibility of anyone looking in between. With a
 * trap gate, two callers can both see the same slot free and both claim
 * it - and the loser's frames are then leaked and the winner's window is
 * silently shared with a stranger.
 *
 * Interrupts off, same reasoning as fs_lock: shm_free_by_owner is on the
 * task-exit path, and the task-exit path is reachable from a timer tick
 * delivering SIGKILL. A lock a dying task can be interrupted while
 * holding is a lock nobody ever unlocks.
 *
 * The pmm/vmm calls made while holding this take their own locks, and
 * always in that order (shm -> pmm, shm -> vmm), never the reverse -
 * neither pmm nor vmm has any reason to know shm exists. */
static spinlock_t shm_lock;

static int find_free_slot(void) {
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
        if (!segments[i].used) {
            return i;
        }
    }
    return -1;
}

int shm_create(size_t size, int owner_task_id) {
    if (size == 0) {
        return -1;
    }
    uint64_t flags = spin_lock_irqsave(&shm_lock);
    int id = find_free_slot();
    if (id < 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }
    /* Claim the slot before dropping into the allocation loop below, so a
     * second caller arriving mid-allocation cannot pick the same one.
     * Everything else about the entry is filled in at the bottom; `used`
     * alone is what find_free_slot consults. */
    segments[id].used = 1;

    uint64_t page_count = ((uint64_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *frames = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
    if (!frames) {
        segments[id].used = 0;
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }

    /* M29: pmm_try_alloc_frame, not pmm_alloc_frame - a shm segment's size
     * is caller-controlled (SYS_shm_create's `size` argument), so a large
     * enough request from a user process could exhaust physical memory
     * mid-loop; that has to fail this call, not panic the whole kernel.
     * Frees whatever this call itself allocated before the shortfall
     * (there's no earlier owner to leave anything usefully mapped for,
     * unlike SYS_sbrk growing an already-live heap) so a failed
     * shm_create never leaks partial state into the segment table. */
    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t phys = pmm_try_alloc_frame();
        if (phys == 0) {
            for (uint64_t j = 0; j < i; j++) {
                pmm_free_frame(frames[j]);
            }
            kfree(frames);
            segments[id].used = 0;
            spin_unlock_irqrestore(&shm_lock, flags);
            return -1;
        }
        k_memset((void *)phys, 0, PAGE_SIZE);
        frames[i] = phys;
    }

    segments[id].owner_task_id = owner_task_id;
    segments[id].size = (uint64_t)size;
    segments[id].page_count = page_count;
    segments[id].frames = frames;
    spin_unlock_irqrestore(&shm_lock, flags);
    return id;
}

void shm_free_by_owner(int owner_task_id) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
        if (!segments[i].used || segments[i].owner_task_id != owner_task_id) {
            continue;
        }
        for (uint64_t p = 0; p < segments[i].page_count; p++) {
            pmm_free_frame(segments[i].frames[p]);
        }
        kfree(segments[i].frames);
        segments[i].used = 0;
        segments[i].frames = (uint64_t *)0;
    }
    spin_unlock_irqrestore(&shm_lock, flags);
}

int shm_free(int id, int owner_task_id) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);
    if (id < 0 || id >= MAX_SHM_SEGMENTS || !segments[id].used) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }
    if (segments[id].owner_task_id != owner_task_id) {
        /* Not a permission model - this project has none - but the one
         * check that keeps "exactly one owner is responsible for
         * releasing it" true rather than aspirational. */
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }
    for (uint64_t p = 0; p < segments[id].page_count; p++) {
        pmm_free_frame(segments[id].frames[p]);
    }
    kfree(segments[id].frames);
    segments[id].used = 0;
    segments[id].frames = (uint64_t *)0;
    spin_unlock_irqrestore(&shm_lock, flags);
    return 0;
}

int64_t shm_page_count(int id) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);
    int64_t r = -1;
    if (id >= 0 && id < MAX_SHM_SEGMENTS && segments[id].used) {
        r = (int64_t)segments[id].page_count;
    }
    spin_unlock_irqrestore(&shm_lock, flags);
    return r;
}

int shm_count_by_owner(int owner_task_id) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);
    int n = 0;
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
        if (segments[i].used && segments[i].owner_task_id == owner_task_id) {
            n++;
        }
    }
    spin_unlock_irqrestore(&shm_lock, flags);
    return n;
}

int64_t shm_get_size(int id) {
    uint64_t flags = spin_lock_irqsave(&shm_lock);
    int64_t r = -1;
    if (id >= 0 && id < MAX_SHM_SEGMENTS && segments[id].used) {
        r = (int64_t)segments[id].size;
    }
    spin_unlock_irqrestore(&shm_lock, flags);
    return r;
}

int shm_map_into(int id, uint64_t pml4_phys, uint64_t vaddr, uint64_t flags) {
    uint64_t irqf = spin_lock_irqsave(&shm_lock);
    if (id < 0 || id >= MAX_SHM_SEGMENTS || !segments[id].used) {
        spin_unlock_irqrestore(&shm_lock, irqf);
        return -1;
    }
    shm_segment_t *seg = &segments[id];
    for (uint64_t i = 0; i < seg->page_count; i++) {
        vmm_map_page_in(pml4_phys, vaddr + i * PAGE_SIZE, seg->frames[i], flags);
    }
    spin_unlock_irqrestore(&shm_lock, irqf);
    return 0;
}
