#include "pmm.h"

#include <stdint.h>

#include "drivers/klog.h"
#include "lib/spinlock.h"
#include "mm/e820.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL

/* Below 1 MiB lives the real-mode IVT/BDA and legacy VGA text memory
 * (0xB8000) - firmware- and architecture-owned regardless of boot path.
 * None of it is safe to hand out as a free frame, regardless of what the
 * memory map says. */
#define LOW_MEMORY_LIMIT 0x100000ULL

/* Where the boot loader puts the kernel (kernel/linker.ld, and
 * KERNEL_LOAD_ADDR in kernel/boot/uefi/boot.c). Named here because M90's
 * metadata placement has to avoid the loaded image, and "0x100000" would
 * otherwise appear as a bare number twice in one function. */
#define KERNEL_LOAD_ADDR 0x100000ULL

/* Provided by linker.ld: end of the kernel's loaded image (.text through
 * .bss), so the kernel can't be handed a frame it's currently running out
 * of or storing data in. */
extern uint8_t __kernel_end[];

/* ---- M90: the bitmap and the refcounts are no longer static arrays ----
 *
 * They were, and the sizes were the reason this allocator could not track
 * more than a gigabyte: a bitmap over 1 GiB is 32 KiB of .bss and a
 * refcount byte per frame is 256 KiB, which is already most of what this
 * kernel's .bss weighs. The same two arrays over 8 GiB are 256 KiB and
 * 2 MiB, and .bss is not where that belongs - it is loaded (or, for
 * NOBITS, zeroed) by the boot loader, it is sized at link time, and it
 * has to be sized for the largest machine anyone might boot on rather
 * than for the machine actually booted.
 *
 * So they are placed at run time, in physical memory, out of the very map
 * they are about to describe. That is the awkward ordering the old header
 * comment predicted, and it is untied by doing the reads before any of
 * the writes: pass one only measures (how much RAM is there, how big does
 * the metadata have to be, where does it fit), and nothing is written
 * anywhere until a home has been chosen that provably overlaps neither
 * the kernel image, nor low memory, nor the e820 buffer itself - which is
 * an EfiLoaderData allocation, therefore inside a range this map calls
 * usable, therefore a place the metadata would otherwise happily land on
 * top of while still reading it. */
static uint8_t *bitmap;
static uint64_t bitmap_bytes;

/* ---- M82: how many owners a frame has ---------------------------------
 *
 * Every frame this allocator hands out had exactly one owner until M82,
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
 * One byte per frame. A byte is enough because an owner is an address
 * space and MAX_TASKS is 128, so 255 is not reachable - and the increment
 * panics rather than wrapping if that reasoning ever stops holding,
 * because a wrapped refcount frees a page somebody is still using.
 *
 * Frames reserved at init (low memory, the kernel image) are marked used
 * in the bitmap and never get a refcount, which is right: nothing owns
 * them and nothing may free them. pmm_free_frame on one of those still
 * hits the double-free panic it always did, because their refcount is
 * zero. */
static uint8_t *frame_refs;

static uint64_t free_frames;
static uint64_t total_frames;
/* M90: the highest frame index a 32-bit DMA engine can be handed. */
static uint64_t dma_frames;
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

/* free_range rounds inward (partial leading/trailing frames stay
 * reserved) since a partially-usable frame is not safe to hand out whole. */
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

/* One exclusion the metadata may not be placed on top of. Three of them
 * exist and all three are ranges inside memory the map calls usable,
 * which is exactly why they have to be listed rather than inferred. */
typedef struct {
    uint64_t base, end;
} exclusion_t;

/* Advances `start` past every exclusion it collides with, repeating until
 * a full pass moves it nowhere - one pass is not enough, because stepping
 * over the kernel image can land on the e820 buffer. Returns 0 if the run
 * no longer fits inside [start, entry_end). */
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

    /* ---- Pass one: measure only. Nothing is written until a home for
     * the metadata has been chosen, because every candidate home is
     * memory this loop is still reading from. */
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
    /* The refcount array follows the bitmap in one allocation: two
     * separate placements would need the search below run twice and would
     * have to exclude the first from the second. */
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

    /* ---- Pass two: the first write. From here the map is described. */
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
    /* The e820 buffer stops being read the moment pmm_init returns, so
     * this could in principle be released - it is not, because "a few
     * pages" is not worth a rule that says a firmware structure becomes
     * free at an exact moment and nothing enforces the moment. */
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

/* The scan every allocator entry point shares. `first`/`limit` bound the
 * frame indices considered; the caller holds the lock. */
static uint64_t claim_first_free(uint64_t first, uint64_t limit) {
    for (uint64_t f = first; f < limit; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            frame_refs[f] = 1; /* M82 - one owner, the caller */
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

/* M90: deliberately does NOT move search_hint. The hint means "no frame
 * below this is free", which a scan bounded at 4 GiB cannot establish -
 * and a driver taking a handful of frames at boot has no business
 * rewinding the hint for every allocation after it. */
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

uint64_t pmm_total_frame_count(void) {
    return total_frames;
}

uint64_t pmm_tracked_limit(void) {
    return total_frames * PAGE_SIZE;
}

/* Scans from the very start of the bitmap rather than search_hint: unlike
 * pmm_alloc_frame's single-page case, a multi-frame *contiguous* run can
 * exist entirely below search_hint (freed single frames scattered there
 * don't advance it), and this is only ever called a handful of times at
 * driver init, not on any hot path, so the extra scan cost doesn't matter.
 *
 * M90: bounded at dma_frames rather than total_frames - see pmm.h. */
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
    spin_unlock_irqrestore(&pmm_lock, irq_flags);
    return 0;
}

/* M102: the panicking form, for the callers that genuinely cannot carry
 * on - and there are none left in this kernel. It is kept because the
 * pattern is the point: pmm_alloc_frame beside pmm_try_alloc_frame, and
 * this beside pmm_try_alloc_contiguous, so a call site says which kind of
 * caller it is. A kernel stack for a new task was the last user of this
 * one, and a spawn that cannot get a stack is an ordinary failed spawn. */
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
