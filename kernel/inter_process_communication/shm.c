#include "shm.h"

#include "library/libk.h"
#include "memory_management/heap.h"
#include "memory_management/pmm.h"
#include "memory_management/vmm.h"
#include "library/spinlock.h"

#define PAGE_SIZE 4096ULL
#define MAX_SHM_SEGMENTS 32

typedef struct {
    int used;
    int owner_task_id;
    uint64_t size;
    uint64_t page_count;
    uint64_t *frames;
} shm_segment_t;

static shm_segment_t segments[MAX_SHM_SEGMENTS];

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
    if ((uint64_t)size > SHM_MAX_SEGMENT_BYTES) {
        return -1;
    }
    uint64_t flags = spin_lock_irqsave(&shm_lock);
    int id = find_free_slot();
    if (id < 0) {
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }
    segments[id].used = 1;

    uint64_t page_count = ((uint64_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *frames = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
    if (!frames) {
        segments[id].used = 0;
        spin_unlock_irqrestore(&shm_lock, flags);
        return -1;
    }

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
        if (vmm_try_map_page_in(pml4_phys, vaddr + i * PAGE_SIZE,
                                seg->frames[i], flags) != 0) {
            spin_unlock_irqrestore(&shm_lock, irqf);
            return -1;
        }
    }
    spin_unlock_irqrestore(&shm_lock, irqf);
    return 0;
}
