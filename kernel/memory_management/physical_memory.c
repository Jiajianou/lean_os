#include "physical_memory.h"

#include <stdint.h>

#include "drivers/kernel_log.h"
#include "library/spinlock.h"
#include "memory_management/e820.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL

#define LOW_MEMORY_LIMIT 0x100000ULL

#define KERNEL_LOAD_ADDR 0x100000ULL

extern uint8_t __kernel_end[];

static uint8_t *bitmap;
static uint64_t bitmap_bytes;

static uint8_t *frame_refs;

static uint64_t free_frames;
static uint64_t total_frames;
static uint64_t dma_frames;
static uint64_t search_hint;

static spinlock_t pmm_lock;

static inline void bitmap_set(uint64_t frame) {
    bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void bitmap_clear(uint64_t frame) {
    bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static inline int bitmap_test(uint64_t frame) {
    return (bitmap[frame / 8] >> (frame % 8)) & 1;
}

static inline uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

static void reserve_range(uint64_t start, uint64_t end) {
    uint64_t limit = total_frames * PAGE_SIZE;
    if (end > limit) end = limit;
    if (start >= end) return;
    uint64_t first = start / PAGE_SIZE;
    uint64_t last = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = first; f < last; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            free_frames--;
        }
    }
}

static void free_range(uint64_t start, uint64_t end) {
    uint64_t limit = total_frames * PAGE_SIZE;
    if (end > limit) end = limit;
    if (start >= end) return;
    uint64_t first = (start + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t last = end / PAGE_SIZE;
    for (uint64_t f = first; f < last; f++) {
        if (bitmap_test(f)) {
            bitmap_clear(f);
            free_frames++;
        }
    }
}

typedef struct {
    uint64_t base, end;
} exclusion_t;

static uint64_t place_after_exclusions(uint64_t start, uint64_t entry_end, uint64_t bytes,
                                       const exclusion_t *excl, int excl_count) {
    for (int pass = 0; pass <= excl_count; pass++) {
        int moved = 0;
        for (int i = 0; i < excl_count; i++) {
            if (start < excl[i].end && excl[i].base < start + bytes) {
                start = align_up(excl[i].end, PAGE_SIZE);
                moved = 1;
            }
        }
        if (!moved) {
            return (start + bytes <= entry_end) ? start : 0;
        }
        if (start + bytes > entry_end) {
            return 0;
        }
    }
    return 0;
}

void pmm_init(const uint32_t *e820_map) {
    uint32_t count = e820_count(e820_map);
    const e820_entry_t *entries = e820_entries(e820_map);

    uint64_t highest_usable_end = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].type != E820_TYPE_USABLE) continue;
        uint64_t end = entries[i].base + entries[i].length;
        if (end > highest_usable_end) highest_usable_end = end;
    }
    if (highest_usable_end <= LOW_MEMORY_LIMIT) {
        panic("pmm_init: no usable memory found");
    }

    total_frames = highest_usable_end / PAGE_SIZE;
    dma_frames = total_frames;
    if (dma_frames > PMM_DMA_LIMIT / PAGE_SIZE) {
        dma_frames = PMM_DMA_LIMIT / PAGE_SIZE;
    }

    bitmap_bytes = (total_frames + 7) / 8;
    uint64_t meta_bytes = align_up(bitmap_bytes, 8) + total_frames;
    meta_bytes = align_up(meta_bytes, PAGE_SIZE);

    exclusion_t excl[3];
    excl[0].base = 0;
    excl[0].end = LOW_MEMORY_LIMIT;
    excl[1].base = KERNEL_LOAD_ADDR;
    excl[1].end = align_up((uint64_t)(uintptr_t)__kernel_end, PAGE_SIZE);
    excl[2].base = (uint64_t)(uintptr_t)e820_map;
    excl[2].end = align_up(excl[2].base + 8 + (uint64_t)count * sizeof(e820_entry_t), PAGE_SIZE);

    uint64_t meta_base = 0;
    for (uint32_t i = 0; i < count && meta_base == 0; i++) {
        if (entries[i].type != E820_TYPE_USABLE) continue;
        uint64_t entry_end = entries[i].base + entries[i].length;
        if (entry_end < meta_bytes) continue;
        meta_base = place_after_exclusions(align_up(entries[i].base, PAGE_SIZE), entry_end,
                                           meta_bytes, excl, 3);
    }
    if (meta_base == 0) {
        panic("pmm_init: no usable range large enough to hold the frame bitmap");
    }

    bitmap = (uint8_t *)(uintptr_t)meta_base;
    frame_refs = bitmap + align_up(bitmap_bytes, 8);

    free_frames = 0;
    search_hint = 0;
    for (uint64_t i = 0; i < bitmap_bytes; i++) {
        bitmap[i] = 0xFF;
    }
    for (uint64_t i = 0; i < total_frames; i++) {
        frame_refs[i] = 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].type == E820_TYPE_USABLE) {
            free_range(entries[i].base, entries[i].base + entries[i].length);
        }
    }

    reserve_range(excl[0].base, excl[0].end);
    reserve_range(excl[1].base, excl[1].end);
    reserve_range(excl[2].base, excl[2].end);
    reserve_range(meta_base, meta_base + meta_bytes);

    if (free_frames == 0) {
        panic("pmm_init: no usable memory found");
    }

    klog_puts("[pmm] ");
    klog_put_hex64(free_frames);
    klog_puts(" / ");
    klog_put_hex64(total_frames);
    klog_puts(" frames free, tracking to 0x");
    klog_put_hex64(highest_usable_end);
    klog_puts(" (");
    klog_put_hex64(highest_usable_end / (1024 * 1024));
    klog_puts(" MiB); bitmap+refcounts at 0x");
    klog_put_hex64(meta_base);
    klog_putc('\n');
}

static uint64_t claim_first_free(uint64_t first, uint64_t limit) {
    for (uint64_t f = first; f < limit; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            frame_refs[f] = 1;
            free_frames--;
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_try_alloc_frame(void) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t phys = claim_first_free(search_hint, total_frames);
    if (phys != 0) {
        search_hint = phys / PAGE_SIZE + 1;
    }
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
    return phys;
}

uint64_t pmm_alloc_frame(void) {
    uint64_t phys = pmm_try_alloc_frame();
    if (phys == 0) {
        panic("pmm_alloc_frame: out of physical memory");
    }
    return phys;
}

uint64_t pmm_alloc_frame_dma(void) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t phys = claim_first_free(0, dma_frames);
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
    if (phys == 0) {
        panic("pmm_alloc_frame_dma: no free frame below 4 GiB");
    }
    return phys;
}

uint64_t pmm_alloc_frame_above(uint64_t min_phys) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t phys = claim_first_free(min_phys / PAGE_SIZE, total_frames);
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
    return phys;
}

void pmm_free_frame(uint64_t phys_addr) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t f = phys_addr / PAGE_SIZE;
    if (f >= total_frames || !bitmap_test(f) || frame_refs[f] == 0) {
        panic("pmm_free_frame: double-free or invalid frame");
    }
    if (--frame_refs[f] == 0) {
        bitmap_clear(f);
        free_frames++;
        if (f < search_hint) {
            search_hint = f;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
}

uint64_t pmm_free_frame_count(void) {
    return free_frames;
}

uint64_t pmm_total_frame_count(void) {
    return total_frames;
}

uint64_t pmm_tracked_limit(void) {
    return total_frames * PAGE_SIZE;
}

uint64_t pmm_try_alloc_contiguous(uint64_t count) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t run_start = 0;
    uint64_t run_len = 0;
    for (uint64_t f = 0; f < dma_frames; f++) {
        if (!bitmap_test(f)) {
            if (run_len == 0) {
                run_start = f;
            }
            run_len++;
            if (run_len == count) {
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_set(run_start + i);
                    frame_refs[run_start + i] = 1;
                }
                free_frames -= count;
                if (run_start <= search_hint && search_hint < run_start + count) {
                    search_hint = run_start + count;
                }
                spin_unlock_irqrestore(&pmm_lock, irq_flags);
                return run_start * PAGE_SIZE;
            }
        } else {
            run_len = 0;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
    return 0;
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
    uint64_t phys = pmm_try_alloc_contiguous(count);
    if (!phys) {
        panic("pmm_alloc_contiguous: no contiguous run of that size found");
    }
    return phys;
}

void pmm_free_contiguous(uint64_t phys_addr, uint64_t count) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t first = phys_addr / PAGE_SIZE;
    if (first + count > total_frames) {
        panic("pmm_free_contiguous: invalid range");
    }
    for (uint64_t f = first; f < first + count; f++) {
        if (!bitmap_test(f) || frame_refs[f] == 0) {
            panic("pmm_free_contiguous: double-free or invalid frame");
        }
        if (--frame_refs[f] == 0) {
            bitmap_clear(f);
            free_frames++;
        }
    }
    if (first < search_hint) {
        search_hint = first;
    }
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
}

void pmm_frame_ref(uint64_t phys_addr) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t f = phys_addr / PAGE_SIZE;
    if (f >= total_frames || !bitmap_test(f) || frame_refs[f] == 0) {
        panic("pmm_frame_ref: no such allocated frame");
    }
    if (frame_refs[f] == 0xFF) {
        panic("pmm_frame_ref: frame owner count would overflow");
    }
    frame_refs[f]++;
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
}

uint8_t pmm_frame_refs(uint64_t phys_addr) {
    uint64_t f = phys_addr / PAGE_SIZE;
    if (f >= total_frames) {
        return 0;
    }
    return frame_refs[f];
}
