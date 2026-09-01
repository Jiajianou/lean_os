/* tests/fakes/fake_pmm.c - Q2
 *
 * A physical frame allocator backed by host malloc.
 *
 * The frames it hands out are real, writable, 4 KiB-aligned host memory,
 * so code under test that maps a frame and writes through it does exactly
 * what it does on the machine. What is faked is where the addresses come
 * from - not what they are.
 *
 * Two things it adds that the real one cannot:
 *
 *   1. A settable failure point. fake_pmm_fail_after(n) makes the n+1'th
 *      allocation fail, which is how a test reaches "the machine ran out
 *      of memory" without a machine that has run out of memory. That is
 *      the branch Q9 is about and the one nothing has ever executed.
 *   2. A leak assertion. fake_pmm_outstanding() is the count of frames
 *      allocated and not freed, so a test can assert an operation is
 *      frame-neutral - the host-side version of the move M50 and M54
 *      already make on the machine.
 *
 * pmm_alloc_frame() keeps the real one's contract exactly: it panics
 * rather than returning 0. pmm_try_alloc_frame() is the one that returns
 * 0, and the difference between them is precisely what Q9 is about. */
#include "mm/pmm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void panic(const char *msg);

#define MAX_FRAMES 8192
#define FRAME_SIZE 4096

typedef struct {
    void *base;
    uint8_t refs;
    int live;
} frame_t;

static frame_t frames[MAX_FRAMES];
static uint64_t outstanding;
static uint64_t total_allocs;
static int64_t fail_after = -1;   /* -1: never fail */

/* Test-visible controls, declared in tests/fakes/fakes.h. */
void fake_pmm_reset(void);
void fake_pmm_fail_after(int64_t n);
uint64_t fake_pmm_outstanding(void);
uint64_t fake_pmm_total_allocs(void);

void fake_pmm_reset(void) {
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

void fake_pmm_fail_after(int64_t n) { fail_after = n; }
uint64_t fake_pmm_outstanding(void) { return outstanding; }
uint64_t fake_pmm_total_allocs(void) { return total_allocs; }

static int slot_of(uint64_t phys) {
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (frames[i].live && (uint64_t)(uintptr_t)frames[i].base == phys) {
            return i;
        }
    }
    return -1;
}

uint64_t pmm_try_alloc_frame(void) {
    if (fail_after >= 0 && (int64_t)total_allocs >= fail_after) {
        return 0;
    }
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (!frames[i].live) {
            void *p = NULL;
            if (posix_memalign(&p, FRAME_SIZE, FRAME_SIZE) != 0 || !p) {
                return 0;
            }
            /* Poisoned rather than zeroed, on purpose. The real allocator
             * makes no promise about a fresh frame's contents, so code
             * that depends on one being zero is wrong and should fail
             * here rather than on hardware that happens to oblige. */
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

uint64_t pmm_alloc_frame(void) {
    uint64_t f = pmm_try_alloc_frame();
    if (!f) {
        /* The real contract, kept verbatim - including the wording, so a
         * CHECK_PANIC in a test matches the same substring it would on
         * the machine. */
        panic("pmm_alloc_frame: out of physical memory");
    }
    return f;
}

uint64_t pmm_alloc_frame_dma(void) { return pmm_alloc_frame(); }

uint64_t pmm_alloc_frame_above(uint64_t min_phys) {
    (void)min_phys;
    return pmm_try_alloc_frame();
}

void pmm_free_frame(uint64_t phys_addr) {
    int i = slot_of(phys_addr);
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

void pmm_frame_ref(uint64_t phys_addr) {
    int i = slot_of(phys_addr);
    if (i < 0) {
        panic("pmm_frame_ref: no such allocated frame");
    }
    if (frames[i].refs == 255) {
        panic("pmm_frame_ref: frame owner count would overflow");
    }
    frames[i].refs++;
}

uint8_t pmm_frame_refs(uint64_t phys_addr) {
    int i = slot_of(phys_addr);
    return i < 0 ? 0 : frames[i].refs;
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
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
    panic("pmm_alloc_contiguous: no contiguous run of that size found");
    return 0;
}

void pmm_free_contiguous(uint64_t phys_addr, uint64_t count) {
    (void)count;
    pmm_free_frame(phys_addr);
}

void pmm_init(const uint32_t *e820_map) { (void)e820_map; }
uint64_t pmm_free_frame_count(void) { return MAX_FRAMES - outstanding; }
uint64_t pmm_total_frame_count(void) { return MAX_FRAMES; }
uint64_t pmm_tracked_limit(void) { return (uint64_t)MAX_FRAMES * FRAME_SIZE; }
