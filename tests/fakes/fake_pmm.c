#include "memory_management/physical_memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void panic(const char *message);

#define MAX_FRAMES 16384
#define FRAME_SIZE 4096

typedef struct {
    void *base;
    uint8_t refs;
    int live;
} frame_t;

static frame_t frames[MAX_FRAMES];
static uint64_t outstanding;
static uint64_t total_allocs;
static int64_t fail_after = -1;

void fake_physical_memory_reset(void);
void fake_physical_memory_fail_after(int64_t n);
uint64_t fake_physical_memory_outstanding(void);
uint64_t fake_physical_memory_total_allocs(void);

void fake_physical_memory_reset(void) {
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (frames[i].live) {
            free(frames[i].base);
        }
        frames[i].base = NULL;
        frames[i].live = 0;
        frames[i].refs = 0;
    }
    outstanding = 0;
    total_allocs = 0;
    fail_after = -1;
}

void fake_physical_memory_fail_after(int64_t n) { fail_after = n; }
uint64_t fake_physical_memory_outstanding(void) { return outstanding; }
uint64_t fake_physical_memory_total_allocs(void) { return total_allocs; }

static int slot_of(uint64_t phys) {
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (frames[i].live && (uint64_t)(uintptr_t)frames[i].base == phys) {
            return i;
        }
    }
    return -1;
}

static void (*alloc_hook)(void);

static void clear_alloc_hook(void) {
    alloc_hook = 0;
}

void fake_spinlock_at_test_end(void (*clear)(void));
void fake_physical_memory_on_alloc(void (*hook)(void));
void fake_physical_memory_on_alloc(void (*hook)(void)) {
    fake_spinlock_at_test_end(clear_alloc_hook);
    alloc_hook = hook;
}

uint64_t physical_memory_try_alloc_frame(void) {
    if (alloc_hook) {
        void (*hook)(void) = alloc_hook;
        alloc_hook = 0;
        hook();
    }
    if (fail_after >= 0 && (int64_t)total_allocs >= fail_after) {
        return 0;
    }
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (!frames[i].live) {
            void *p = NULL;
            if (posix_memalign(&p, FRAME_SIZE, FRAME_SIZE) != 0 || !p) {
                return 0;
            }
            memset(p, 0xCD, FRAME_SIZE);
            frames[i].base = p;
            frames[i].live = 1;
            frames[i].refs = 1;
            outstanding++;
            total_allocs++;
            return (uint64_t)(uintptr_t)p;
        }
    }
    return 0;
}

uint64_t physical_memory_alloc_frame(void) {
    uint64_t f = physical_memory_try_alloc_frame();
    if (!f) {
        panic("pmm_alloc_frame: out of physical memory");
    }
    return f;
}

uint64_t physical_memory_alloc_frame_dma(void) { return physical_memory_alloc_frame(); }

uint64_t physical_memory_alloc_frame_above(uint64_t min_phys) {
    (void)min_phys;
    return physical_memory_try_alloc_frame();
}

void physical_memory_free_frame(uint64_t phys_address) {
    int i = slot_of(phys_address);
    if (i < 0) {
        panic("pmm_free_frame: double-free or invalid frame");
    }
    if (frames[i].refs > 1) {
        frames[i].refs--;
        return;
    }
    free(frames[i].base);
    frames[i].base = NULL;
    frames[i].live = 0;
    frames[i].refs = 0;
    outstanding--;
}

void physical_memory_frame_reference(uint64_t phys_address) {
    int i = slot_of(phys_address);
    if (i < 0) {
        panic("pmm_frame_ref: no such allocated frame");
    }
    if (frames[i].refs == 255) {
        panic("pmm_frame_ref: frame owner count would overflow");
    }
    frames[i].refs++;
}

uint8_t physical_memory_frame_refs(uint64_t phys_address) {
    int i = slot_of(phys_address);
    return i < 0 ? 0 : frames[i].refs;
}

static uint64_t alloc_contiguous(uint64_t count, int may_fail) {
    if (count == 0) {
        return 0;
    }
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (frames[i].live) {
            continue;
        }
        void *p = NULL;
        if (posix_memalign(&p, FRAME_SIZE, (size_t)(count * FRAME_SIZE)) != 0 || !p) {
            panic("pmm_alloc_contiguous: no contiguous run of that size found");
        }
        memset(p, 0xCD, (size_t)(count * FRAME_SIZE));
        frames[i].base = p;
        frames[i].live = 1;
        frames[i].refs = 1;
        outstanding++;
        total_allocs++;
        return (uint64_t)(uintptr_t)p;
    }
    if (may_fail) {
        return 0;
    }
    panic("pmm_alloc_contiguous: no contiguous run of that size found");
    return 0;
}

uint64_t physical_memory_alloc_contiguous(uint64_t count) {
    return alloc_contiguous(count, 0);
}

void physical_memory_free_contiguous(uint64_t phys_address, uint64_t count) {
    (void)count;
    physical_memory_free_frame(phys_address);
}

uint64_t physical_memory_try_alloc_contiguous(uint64_t count) {
    if (fail_after >= 0 && (int64_t)total_allocs >= fail_after) {
        return 0;
    }
    return alloc_contiguous(count, 1);
}

uint64_t physical_memory_try_alloc_contiguous_anywhere(uint64_t count) {
    return physical_memory_try_alloc_contiguous(count);
}

void physical_memory_init(const uint32_t *e820_map) { (void)e820_map; }
uint64_t physical_memory_free_frame_count(void) { return MAX_FRAMES - outstanding; }
uint64_t physical_memory_total_frame_count(void) { return MAX_FRAMES; }
uint64_t physical_memory_tracked_limit(void) { return (uint64_t)MAX_FRAMES * FRAME_SIZE; }
