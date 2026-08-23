#include "shm.h"

#include "lib/libk.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define PAGE_SIZE 4096ULL
#define MAX_SHM_SEGMENTS 16

typedef struct {
    int used;
    int owner_task_id;
    uint64_t size;       /* requested size in bytes, not page-rounded */
    uint64_t page_count;
    uint64_t *frames;    /* kmalloc'd array of page_count physical addresses */
} shm_segment_t;

static shm_segment_t segments[MAX_SHM_SEGMENTS];

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
    int id = find_free_slot();
    if (id < 0) {
        return -1;
    }

    uint64_t page_count = ((uint64_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *frames = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
    if (!frames) {
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
            return -1;
        }
        k_memset((void *)phys, 0, PAGE_SIZE);
        frames[i] = phys;
    }

    segments[id].used = 1;
    segments[id].owner_task_id = owner_task_id;
    segments[id].size = (uint64_t)size;
    segments[id].page_count = page_count;
    segments[id].frames = frames;
    return id;
}

void shm_free_by_owner(int owner_task_id) {
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
}

int64_t shm_get_size(int id) {
    if (id < 0 || id >= MAX_SHM_SEGMENTS || !segments[id].used) {
        return -1;
    }
    return (int64_t)segments[id].size;
}

int shm_map_into(int id, uint64_t pml4_phys, uint64_t vaddr, uint64_t flags) {
    if (id < 0 || id >= MAX_SHM_SEGMENTS || !segments[id].used) {
        return -1;
    }
    shm_segment_t *seg = &segments[id];
    for (uint64_t i = 0; i < seg->page_count; i++) {
        vmm_map_page_in(pml4_phys, vaddr + i * PAGE_SIZE, seg->frames[i], flags);
    }
    return 0;
}
