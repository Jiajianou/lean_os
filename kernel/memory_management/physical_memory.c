#include "physical_memory.h"

#include <stdint.h>

#include "drivers/kernel_log.h"
#include "library/spinlock.h"
#include "memory_management/e820.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL

#define LOW_MEMORY_LIMIT 0x100000ULL

#define KERNEL_LOAD_ADDRESS 0x100000ULL

extern uint8_t __kernel_end[];

/* Where a physical address is to the code that reads it, the physical
   address of a pointer the code was handed, and where the kernel's image
   ends. On the machine physical memory is identity-mapped and the linker
   says where the image ends, so these are the plain answers.
   tests/test_physical_memory.c runs this file on the host over a firmware
   map whose RAM is a buffer the test owns, and says so by defining all
   three. The middle one is there because the firmware map's own pages are
   reserved by their address, and a host pointer is wherever that host keeps
   its stack: far above any map on an x86_64 Mac, but about 6 GiB on an
   arm64 Mac's main thread - inside the test's 4-17 GiB of RAM - so without
   it the count of reserved frames would depend on which Mac ran the test. */
#ifndef PHYSICAL_MEMORY_POINTER
#define PHYSICAL_MEMORY_POINTER(physical) ((uint8_t *)(uintptr_t)(physical))
#endif
#ifndef PHYSICAL_MEMORY_ADDRESS_OF
#define PHYSICAL_MEMORY_ADDRESS_OF(pointer) ((uint64_t)(uintptr_t)(pointer))
#endif
#ifndef PHYSICAL_MEMORY_KERNEL_END
#define PHYSICAL_MEMORY_KERNEL_END ((uint64_t)(uintptr_t)__kernel_end)
#endif

static uint64_t metadata_end;
static uint8_t *bitmap;
static uint64_t bitmap_bytes;

static uint8_t *frame_refs;

static uint64_t free_frames;

/* Two different numbers, and until they were two the machine misreported
   itself. tracked_frames is the SPAN: the highest usable address divided by
   a page, which is how far the bitmap and the reference counts have to
   reach, because a frame's index is its address. ram_frames is the MEMORY:
   the frames the firmware handed over as usable. Between them is every hole
   in the map, and on a PC the big one is the PCI window below 4 GiB - QEMU
   puts 3 GiB of a 16 GiB guest under it and 13 above, so the span is
   17 GiB, and "[inventory] memory: 17408 MiB" is what a 16 GiB machine
   said. The ThinkPad M188 booted on has 16 GB and was written down as 18.
   physical_memory_total_frame_count() is the memory, and it is what
   sysinfo(2), sysconf(_SC_PHYS_PAGES), Settings and the image cache's
   budget are told; physical_memory_tracked_limit() is the span. */
static uint64_t tracked_frames;
static uint64_t ram_frames;
static uint64_t dma_frames;
static uint64_t search_hint;

static spinlock_t physical_memory_lock;

/* What the free was, for the panic below.

   "pmm_free_frame: double-free or invalid frame" names a frame number and
   nothing else, and a frame number says nothing about which subsystem let go
   of it twice. M167 spent four instrumented boots getting from that message
   to a cause, and M168 spent several more; both times the useful facts were
   the same three - the frame's own state, the SITE that was freeing, and the
   VIRTUAL ADDRESS it was mapped at, which says whether it was a stack, an
   image or the mmap arena. So the panic carries them. */
const char *pmm_free_site = "?";
uint64_t pmm_free_virt = 0;

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
    uint64_t limit = tracked_frames * PAGE_SIZE;
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
    uint64_t limit = tracked_frames * PAGE_SIZE;
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

void physical_memory_init(const uint32_t *e820_map) {
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

    tracked_frames = highest_usable_end / PAGE_SIZE;
    dma_frames = tracked_frames;
    if (dma_frames > PHYSICAL_MEMORY_DMA_LIMIT / PAGE_SIZE) {
        dma_frames = PHYSICAL_MEMORY_DMA_LIMIT / PAGE_SIZE;
    }

    bitmap_bytes = (tracked_frames + 7) / 8;
    uint64_t meta_bytes = align_up(bitmap_bytes, 8) + tracked_frames;
    meta_bytes = align_up(meta_bytes, PAGE_SIZE);

    exclusion_t excl[3];
    excl[0].base = 0;
    excl[0].end = LOW_MEMORY_LIMIT;
    excl[1].base = KERNEL_LOAD_ADDRESS;
    excl[1].end = align_up(PHYSICAL_MEMORY_KERNEL_END, PAGE_SIZE);
    excl[2].base = PHYSICAL_MEMORY_ADDRESS_OF(e820_map);
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

    metadata_end = meta_base + meta_bytes;
    bitmap = PHYSICAL_MEMORY_POINTER(meta_base);
    frame_refs = bitmap + align_up(bitmap_bytes, 8);

    free_frames = 0;
    search_hint = 0;
    for (uint64_t i = 0; i < bitmap_bytes; i++) {
        bitmap[i] = 0xFF;
    }
    for (uint64_t i = 0; i < tracked_frames; i++) {
        frame_refs[i] = 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].type == E820_TYPE_USABLE) {
            free_range(entries[i].base, entries[i].base + entries[i].length);
        }
    }
    /* Counted here, between marking the usable regions free and taking the
       kernel's own pieces back out of them: free_range counts only a frame
       it actually changes, so two firmware entries that overlap are one
       frame of memory and not two, and a part-page at an entry's edge -
       which the allocator can never hand out - is not memory either. */
    ram_frames = free_frames;

    reserve_range(excl[0].base, excl[0].end);
    reserve_range(excl[1].base, excl[1].end);
    reserve_range(excl[2].base, excl[2].end);
    reserve_range(meta_base, meta_base + meta_bytes);

    if (free_frames == 0) {
        panic("pmm_init: no usable memory found");
    }

    kernel_log_puts("[pmm] ");
    kernel_log_put_hex64(free_frames);
    kernel_log_puts(" / ");
    kernel_log_put_hex64(ram_frames);
    kernel_log_puts(" frames of RAM free, tracking 0x");
    kernel_log_put_hex64(tracked_frames);
    kernel_log_puts(" frames to 0x");
    kernel_log_put_hex64(highest_usable_end);
    kernel_log_puts(" (");
    kernel_log_put_hex64(highest_usable_end / (1024 * 1024));
    kernel_log_puts(" MiB); bitmap+refcounts at 0x");
    kernel_log_put_hex64(meta_base);
    kernel_log_putc('\n');
}

uint64_t physical_memory_metadata_end(void) {
    return metadata_end;
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

uint64_t physical_memory_try_alloc_frame(void) {
    uint64_t irq_flags = spin_lock_irqsave(&physical_memory_lock);
    uint64_t phys = claim_first_free(search_hint, tracked_frames);
    if (phys != 0) {
        search_hint = phys / PAGE_SIZE + 1;
    }
    spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
    return phys;
}

uint64_t physical_memory_alloc_frame(void) {
    uint64_t phys = physical_memory_try_alloc_frame();
    if (phys == 0) {
        panic("pmm_alloc_frame: out of physical memory");
    }
    return phys;
}

uint64_t physical_memory_alloc_frame_dma(void) {
    uint64_t irq_flags = spin_lock_irqsave(&physical_memory_lock);
    uint64_t phys = claim_first_free(0, dma_frames);
    spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
    if (phys == 0) {
        panic("pmm_alloc_frame_dma: no free frame below 4 GiB");
    }
    return phys;
}

uint64_t physical_memory_alloc_frame_above(uint64_t min_phys) {
    uint64_t irq_flags = spin_lock_irqsave(&physical_memory_lock);
    uint64_t phys = claim_first_free(min_phys / PAGE_SIZE, tracked_frames);
    spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
    return phys;
}

void physical_memory_free_frame(uint64_t phys_address) {
    uint64_t irq_flags = spin_lock_irqsave(&physical_memory_lock);
    uint64_t f = phys_address / PAGE_SIZE;
    if (f >= tracked_frames || !bitmap_test(f) || frame_refs[f] == 0) {
        kernel_log_puts("[pmm] free frame 0x");
        kernel_log_put_hex64(phys_address);
        kernel_log_puts(" index 0x");
        kernel_log_put_hex64(f);
        kernel_log_puts(" of 0x");
        kernel_log_put_hex64(tracked_frames);
        kernel_log_puts(" bitmap ");
        kernel_log_put_dec((uint32_t)(f < tracked_frames ? bitmap_test(f) : 0));
        kernel_log_puts(" refs ");
        kernel_log_put_dec((uint32_t)(f < tracked_frames ? frame_refs[f] : 0));
        kernel_log_puts(" site ");
        kernel_log_puts(pmm_free_site);
        kernel_log_puts(" virt 0x");
        kernel_log_put_hex64(pmm_free_virt);
        kernel_log_putc('\n');
        panic("pmm_free_frame: double-free or invalid frame");
    }
    if (--frame_refs[f] == 0) {
        bitmap_clear(f);
        free_frames++;
        if (f < search_hint) {
            search_hint = f;
        }
    }
    spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
}

uint64_t physical_memory_free_frame_count(void) {
    return free_frames;
}

uint64_t physical_memory_total_frame_count(void) {
    return ram_frames;
}

uint64_t physical_memory_tracked_limit(void) {
    return tracked_frames * PAGE_SIZE;
}

uint64_t physical_memory_try_alloc_contiguous(uint64_t count) {
    uint64_t irq_flags = spin_lock_irqsave(&physical_memory_lock);
    uint64_t run_start = 0;
    uint64_t run_length = 0;
    for (uint64_t f = 0; f < dma_frames; f++) {
        if (!bitmap_test(f)) {
            if (run_length == 0) {
                run_start = f;
            }
            run_length++;
            if (run_length == count) {
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_set(run_start + i);
                    frame_refs[run_start + i] = 1;
                }
                free_frames -= count;
                if (run_start <= search_hint && search_hint < run_start + count) {
                    search_hint = run_start + count;
                }
                spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
                return run_start * PAGE_SIZE;
            }
        } else {
            run_length = 0;
        }
    }
    spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
    return 0;
}

/* A contiguous run ANYWHERE, searched from the top of memory down, for
   memory only the kernel touches - a task's kernel stack, the task table -
   which needs to be contiguous and nothing else: the identity map covers
   every frame, so a stack above 4 GiB is as reachable as one below it.

   The search above stops at 4 GiB because its other callers are devices that
   DMA into what they get. Ordinary frames are claimed from the bottom up, so
   the region below 4 GiB is the one that fragments first, and a machine with
   most of its free memory above that line refused a 32 KB kernel stack with
   940 MB free: a Chromium renderer's pthread_create said EAGAIN and it
   stopped on a CHECK (M187). Searching from the top also leaves the low
   region to the devices that need it. */
uint64_t physical_memory_try_alloc_contiguous_anywhere(uint64_t count) {
    if (count == 0) {
        return 0;
    }
    uint64_t irq_flags = spin_lock_irqsave(&physical_memory_lock);
    uint64_t run_length = 0;
    for (uint64_t f = tracked_frames; f-- > 0;) {
        if (bitmap_test(f)) {
            run_length = 0;
            continue;
        }
        run_length++;
        if (run_length == count) {
            uint64_t run_start = f;
            for (uint64_t i = 0; i < count; i++) {
                bitmap_set(run_start + i);
                frame_refs[run_start + i] = 1;
            }
            free_frames -= count;
            if (run_start <= search_hint && search_hint < run_start + count) {
                search_hint = run_start + count;
            }
            spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
            return run_start * PAGE_SIZE;
        }
    }
    spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
    return 0;
}

uint64_t physical_memory_alloc_contiguous(uint64_t count) {
    uint64_t phys = physical_memory_try_alloc_contiguous(count);
    if (!phys) {
        panic("pmm_alloc_contiguous: no contiguous run of that size found");
    }
    return phys;
}

void physical_memory_free_contiguous(uint64_t phys_address, uint64_t count) {
    uint64_t irq_flags = spin_lock_irqsave(&physical_memory_lock);
    uint64_t first = phys_address / PAGE_SIZE;
    if (first + count > tracked_frames) {
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
    spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
}

void physical_memory_frame_reference(uint64_t phys_address) {
    uint64_t irq_flags = spin_lock_irqsave(&physical_memory_lock);
    uint64_t f = phys_address / PAGE_SIZE;
    if (f >= tracked_frames || !bitmap_test(f) || frame_refs[f] == 0) {
        panic("pmm_frame_ref: no such allocated frame");
    }
    if (frame_refs[f] == 0xFF) {
        panic("pmm_frame_ref: frame owner count would overflow");
    }
    frame_refs[f]++;
    spin_unlock_irqrestore(&physical_memory_lock, irq_flags);
}

uint8_t physical_memory_frame_refs(uint64_t phys_address) {
    uint64_t f = phys_address / PAGE_SIZE;
    if (f >= tracked_frames) {
        return 0;
    }
    return frame_refs[f];
}
