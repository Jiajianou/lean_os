#include "pmm.h"

#include <stdint.h>

#include "drivers/klog.h"
#include "lib/spinlock.h"
#include "mm/e820.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL
#define PMM_FRAME_COUNT (PMM_TRACKED_MEMORY / PAGE_SIZE)
#define PMM_BITMAP_BYTES (PMM_FRAME_COUNT / 8)

/* Below 1 MiB lives the real-mode IVT/BDA and legacy VGA text memory
 * (0xB8000) - firmware- and architecture-owned regardless of boot path.
 * None of it is safe to hand out as a free frame, regardless of what the
 * memory map says. */
#define LOW_MEMORY_LIMIT 0x100000ULL

/* Provided by linker.ld: end of the kernel's loaded image (.text through
 * .bss), so the kernel can't be handed a frame it's currently running out
 * of or storing data in. */
extern uint8_t __kernel_end[];

/* 1 = reserved/used, 0 = free. Starts fully reserved; pmm_init clears bits
 * for usable E820 ranges, then re-reserves low memory and the kernel image
 * so a stray/wrong E820 entry can't hand out memory that's actually
 * spoken for. */
static uint8_t bitmap[PMM_BITMAP_BYTES];

/* ---- M82: how many owners a frame has ---------------------------------
 *
 * Every frame this allocator hands out had exactly one owner until now,
 * so "allocated" and "owned by somebody" were the same bit and the
 * bitmap above said both. M83 breaks that: a forked address space shares
 * its parent's pages read-only until one of them writes, and a page with
 * two owners must not come back to the free list when the first of them
 * lets go.
 *
 * So the bitmap keeps meaning "this frame is not free" and this array
 * means "and this many owners are holding it". pmm_alloc_frame hands
 * back a frame with one; pmm_frame_ref adds an owner; pmm_free_frame
 * removes one and only clears the bitmap bit at zero. Every existing
 * caller is a single owner calling free once, which is exactly the old
 * behaviour, so nothing outside this file had to change for it.
 *
 * One byte per frame: 256 KiB for the 1 GiB this allocator tracks. A
 * byte is enough because an owner is an address space and MAX_TASKS is
 * 128, so 255 is not reachable - and the increment panics rather than
 * wrapping if that reasoning ever stops holding, because a wrapped
 * refcount frees a page somebody is still using.
 *
 * Frames reserved at init (low memory, the kernel image) are marked used
 * in the bitmap and never get a refcount, which is right: nothing owns
 * them and nothing may free them. pmm_free_frame on one of those still
 * hits the double-free panic it always did, because their refcount is
 * zero. */
static uint8_t frame_refs[PMM_FRAME_COUNT];
static uint64_t free_frames;
static uint64_t total_frames;
/* First frame index that might still be free. Monotonically advanced by
 * pmm_alloc_frame (and pulled back down by pmm_free_frame) so a long
 * kernel lifetime of allocations doesn't re-scan an ever-growing prefix
 * of known-used frames on every call. */
static uint64_t search_hint;

/* SMP: a bitmap scan-and-claim (or clear-and-credit) is a read-modify-write
 * on shared state - two CPUs calling pmm_alloc_frame concurrently without
 * this could both walk into the same free bit and hand it out twice. A
 * leaf lock: nothing pmm.c itself calls ever takes another kernel lock, so
 * this can never be part of a lock-ordering cycle. */
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

static void reserve_range(uint64_t start, uint64_t end) {
    if (end > PMM_TRACKED_MEMORY) end = PMM_TRACKED_MEMORY;
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

/* free_range rounds inward (partial leading/trailing frames stay
 * reserved) since a partially-usable frame is not safe to hand out whole. */
static void free_range(uint64_t start, uint64_t end) {
    if (end > PMM_TRACKED_MEMORY) end = PMM_TRACKED_MEMORY;
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

void pmm_init(const uint32_t *e820_map) {
    total_frames = PMM_FRAME_COUNT;
    free_frames = 0;
    search_hint = 0;

    for (uint64_t i = 0; i < PMM_BITMAP_BYTES; i++) {
        bitmap[i] = 0xFF;
    }

    uint32_t count = *e820_map;
    const e820_entry_t *entries = (const e820_entry_t *)((const uint8_t *)e820_map + 8);
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].type == E820_TYPE_USABLE) {
            free_range(entries[i].base, entries[i].base + entries[i].length);
        }
    }

    reserve_range(0, LOW_MEMORY_LIMIT);
    reserve_range(0x100000ULL, (uint64_t)(uintptr_t)__kernel_end);

    if (free_frames == 0) {
        panic("pmm_init: no usable memory found");
    }

    klog_puts("[pmm] ");
    klog_put_hex64(free_frames);
    klog_puts(" / ");
    klog_put_hex64(total_frames);
    klog_puts(" frames free (tracking the first 1 GiB only)\n");
}

uint64_t pmm_try_alloc_frame(void) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    for (uint64_t f = search_hint; f < total_frames; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            frame_refs[f] = 1; /* M82 - one owner, the caller */
            free_frames--;
            search_hint = f + 1;
            spin_unlock_irqrestore(&pmm_lock, irq_flags);
            return f * PAGE_SIZE;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
    return 0;
}

uint64_t pmm_alloc_frame(void) {
    uint64_t phys = pmm_try_alloc_frame();
    if (phys == 0) {
        panic("pmm_alloc_frame: out of physical memory");
    }
    return phys;
}

void pmm_free_frame(uint64_t phys_addr) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t f = phys_addr / PAGE_SIZE;
    if (f >= total_frames || !bitmap_test(f) || frame_refs[f] == 0) {
        panic("pmm_free_frame: double-free or invalid frame");
    }
    /* M82: one owner letting go, not necessarily the last. The frame only
     * returns to the free list when nobody is left holding it. */
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

/* Scans from the very start of the bitmap rather than search_hint: unlike
 * pmm_alloc_frame's single-page case, a multi-frame *contiguous* run can
 * exist entirely below search_hint (freed single frames scattered there
 * don't advance it), and this is only ever called a handful of times at
 * driver init, not on any hot path, so the extra scan cost doesn't matter. */
uint64_t pmm_alloc_contiguous(uint64_t count) {
    uint64_t irq_flags = spin_lock_irqsave(&pmm_lock);
    uint64_t run_start = 0;
    uint64_t run_len = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (!bitmap_test(f)) {
            if (run_len == 0) {
                run_start = f;
            }
            run_len++;
            if (run_len == count) {
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_set(run_start + i);
                    frame_refs[run_start + i] = 1; /* M82 */
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
    panic("pmm_alloc_contiguous: no contiguous run of that size found");
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
        /* A contiguous run is a kernel-internal allocation (task stacks)
         * and is never shared, so this decrement always reaches zero -
         * written as a decrement anyway so there is one rule about what
         * frame_refs means rather than two. */
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

/* M82: add an owner to a frame that already has at least one.
 *
 * The only caller is M83's fork, which walks a parent's page tables and
 * points a child's at the same physical pages. Refuses to be the *first*
 * owner - a frame nobody has allocated is not a frame anybody may claim -
 * and panics rather than saturating, because a refcount that stopped
 * counting would eventually free a page two address spaces are still
 * reading. */
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

/* How many owners a frame has, or 0 if it is free. Exists so a self-test
 * can state what sharing *is* rather than infer it from a free-frame
 * count that would look identical whether two address spaces shared one
 * page or each had its own. */
uint8_t pmm_frame_refs(uint64_t phys_addr) {
    uint64_t f = phys_addr / PAGE_SIZE;
    if (f >= total_frames) {
        return 0;
    }
    return frame_refs[f];
}
