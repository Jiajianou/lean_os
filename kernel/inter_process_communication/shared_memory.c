#include "shared_memory.h"

#include "library/kernel_library.h"
#include "memory_management/heap.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "library/spinlock.h"

#define PAGE_SIZE 4096ULL
#define MAX_SHARED_MEMORY_SEGMENTS 32

typedef struct {
    int used;
    int owner_task_id;
    uint64_t size;
    uint64_t page_count;
    uint64_t *frames;
} shared_memory_segment_t;

static shared_memory_segment_t segments[MAX_SHARED_MEMORY_SEGMENTS];

static spinlock_t shared_memory_lock;

static int find_free_slot(void) {
    for (int i = 0; i < MAX_SHARED_MEMORY_SEGMENTS; i++) {
        if (!segments[i].used) {
            return i;
        }
    }
    return -1;
}

int shared_memory_create(size_t size, int owner_task_id) {
    if (size == 0) {
        return -1;
    }
    if ((uint64_t)size > SHARED_MEMORY_MAX_SEGMENT_BYTES) {
        return -1;
    }
    uint64_t flags = spin_lock_irqsave(&shared_memory_lock);
    int id = find_free_slot();
    if (id < 0) {
        spin_unlock_irqrestore(&shared_memory_lock, flags);
        return -1;
    }
    segments[id].used = 1;

    uint64_t page_count = ((uint64_t)size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *frames = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
    if (!frames) {
        segments[id].used = 0;
        spin_unlock_irqrestore(&shared_memory_lock, flags);
        return -1;
    }

    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t phys = physical_memory_try_alloc_frame();
        if (phys == 0) {
            for (uint64_t j = 0; j < i; j++) {
                physical_memory_free_frame(frames[j]);
            }
            kfree(frames);
            segments[id].used = 0;
            spin_unlock_irqrestore(&shared_memory_lock, flags);
            return -1;
        }
        k_memset((void *)phys, 0, PAGE_SIZE);
        frames[i] = phys;
    }

    segments[id].owner_task_id = owner_task_id;
    segments[id].size = (uint64_t)size;
    segments[id].page_count = page_count;
    segments[id].frames = frames;
    spin_unlock_irqrestore(&shared_memory_lock, flags);
    return id;
}

void shared_memory_free_by_owner(int owner_task_id) {
    uint64_t flags = spin_lock_irqsave(&shared_memory_lock);
    for (int i = 0; i < MAX_SHARED_MEMORY_SEGMENTS; i++) {
        if (!segments[i].used || segments[i].owner_task_id != owner_task_id) {
            continue;
        }
        for (uint64_t p = 0; p < segments[i].page_count; p++) {
            physical_memory_free_frame(segments[i].frames[p]);
        }
        kfree(segments[i].frames);
        segments[i].used = 0;
        segments[i].frames = (uint64_t *)0;
    }
    spin_unlock_irqrestore(&shared_memory_lock, flags);
}

int shared_memory_free(int id, int owner_task_id) {
    uint64_t flags = spin_lock_irqsave(&shared_memory_lock);
    if (id < 0 || id >= MAX_SHARED_MEMORY_SEGMENTS || !segments[id].used) {
        spin_unlock_irqrestore(&shared_memory_lock, flags);
        return -1;
    }
    if (segments[id].owner_task_id != owner_task_id) {
        spin_unlock_irqrestore(&shared_memory_lock, flags);
        return -1;
    }
    for (uint64_t p = 0; p < segments[id].page_count; p++) {
        physical_memory_free_frame(segments[id].frames[p]);
    }
    kfree(segments[id].frames);
    segments[id].used = 0;
    segments[id].frames = (uint64_t *)0;
    spin_unlock_irqrestore(&shared_memory_lock, flags);
    return 0;
}

int64_t shared_memory_page_count(int id) {
    uint64_t flags = spin_lock_irqsave(&shared_memory_lock);
    int64_t r = -1;
    if (id >= 0 && id < MAX_SHARED_MEMORY_SEGMENTS && segments[id].used) {
        r = (int64_t)segments[id].page_count;
    }
    spin_unlock_irqrestore(&shared_memory_lock, flags);
    return r;
}

int shared_memory_count_by_owner(int owner_task_id) {
    uint64_t flags = spin_lock_irqsave(&shared_memory_lock);
    int n = 0;
    for (int i = 0; i < MAX_SHARED_MEMORY_SEGMENTS; i++) {
        if (segments[i].used && segments[i].owner_task_id == owner_task_id) {
            n++;
        }
    }
    spin_unlock_irqrestore(&shared_memory_lock, flags);
    return n;
}

int64_t shared_memory_get_size(int id) {
    uint64_t flags = spin_lock_irqsave(&shared_memory_lock);
    int64_t r = -1;
    if (id >= 0 && id < MAX_SHARED_MEMORY_SEGMENTS && segments[id].used) {
        r = (int64_t)segments[id].size;
    }
    spin_unlock_irqrestore(&shared_memory_lock, flags);
    return r;
}

int shared_memory_map_into(int id, uint64_t pml4_phys, uint64_t vaddr, uint64_t flags) {
    uint64_t irqf = spin_lock_irqsave(&shared_memory_lock);
    if (id < 0 || id >= MAX_SHARED_MEMORY_SEGMENTS || !segments[id].used) {
        spin_unlock_irqrestore(&shared_memory_lock, irqf);
        return -1;
    }
    /* M204: every mapping holds a reference on each frame it maps, and the
       segment holds one more. They held none, so the owner freeing a
       segment freed its frames while a client still had them mapped - and a
       window that is resized is exactly that: the compositor freed the old
       buffer at once and Chromium's raster threads went on drawing into it,
       into frames the allocator had already handed to a renderer's heap or
       the browser's own. A frame goes back now when the last of the owner
       and its mappers lets go. */
    shared_memory_segment_t *seg = &segments[id];
    for (uint64_t i = 0; i < seg->page_count; i++) {
        physical_memory_frame_reference(seg->frames[i]);
        if (virtual_memory_try_map_page_in(pml4_phys, vaddr + i * PAGE_SIZE,
                                seg->frames[i], flags) != 0) {
            physical_memory_free_frame(seg->frames[i]);
            for (uint64_t j = 0; j < i; j++) {
                uint64_t taken = virtual_memory_unmap_page_take(pml4_phys, vaddr + j * PAGE_SIZE);
                if (taken) {
                    physical_memory_free_frame(taken);
                }
            }
            spin_unlock_irqrestore(&shared_memory_lock, irqf);
            return -1;
        }
    }
    spin_unlock_irqrestore(&shared_memory_lock, irqf);
    return 0;
}

uint64_t shared_memory_unmap_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t pages) {
    uint64_t released[64];
    uint64_t count = 0;
    uint64_t dropped = 0;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t taken = virtual_memory_unmap_page_take(pml4_phys, vaddr + i * PAGE_SIZE);
        if (!taken) {
            continue;
        }
        released[count++] = taken;
        if (count == sizeof(released) / sizeof(released[0])) {
            virtual_memory_flush_other_cpus(pml4_phys);
            for (uint64_t k = 0; k < count; k++) {
                physical_memory_free_frame(released[k]);
            }
            dropped += count;
            count = 0;
        }
    }
    if (count) {
        virtual_memory_flush_other_cpus(pml4_phys);
        for (uint64_t k = 0; k < count; k++) {
            physical_memory_free_frame(released[k]);
        }
        dropped += count;
    }
    return dropped;
}
