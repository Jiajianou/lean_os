#include "heap.h"

#include <stddef.h>
#include <stdint.h>

#include "drivers/klog.h"
#include "lib/spinlock.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL
#define HEAP_ALIGN 8ULL /* word alignment - sufficient for every scalar type in use so far, no SSE/AVX allocations yet */

/* An address above every identity-mapped range, so every page this heap
 * grows into is a genuinely new vmm_map_page mapping (not already-present
 * identity-map structure) - a real exercise of the vmm map path, not a
 * free ride on the bootstrap map.
 *
 * M90: this used to be PMM_TRACKED_MEMORY, i.e. "just above the 1 GiB the
 * identity map covers". That derivation was right while the identity map
 * was a constant and became wrong the moment it was sized from the
 * machine: a heap that moves when the RAM does is a heap that lands
 * inside the identity map on a machine with 2 GiB. vmm.h now names a
 * fixed address well clear of both. */
/* Q2: vmm_kernel_heap_base() rather than the constant. Same value on the
 * machine; see vmm.h for why it is asked for rather than expanded here. */

typedef struct block_header {
    size_t size; /* usable bytes following this header - excludes the header itself */
    int free;
    struct block_header *next;
} block_header_t;

static uint64_t heap_virt_end; /* one past the last mapped heap byte */
static block_header_t *heap_head;

/* SMP: guards the whole free list plus heap_virt_end - kmalloc/kfree walk
 * and mutate the list (and, on growth, extend the mapped range) as one
 * multi-step operation that has to look atomic to another CPU doing the
 * same. Held across grow_heap's own pmm_alloc_frame/vmm_map_page calls,
 * so lock order is always heap_lock -> {pmm_lock, vmm_lock}, never the
 * other way around anywhere in this kernel - no cycle, no deadlock risk. */
static spinlock_t heap_lock;

static uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

/* Maps `pages` more frames onto the end of the heap's virtual range and
 * returns a pointer to the start of that new space, or NULL if the
 * machine has no memory left. The heap only ever grows (no page is ever
 * unmapped on kfree) - reclaiming address space is future work once
 * something actually needs it.
 *
 * ---- M102: what a partial failure has to do -------------------------
 *
 * Growing by eight pages and failing on the sixth used to be impossible,
 * because the allocator halted the machine instead. Now it is an ordinary
 * outcome, and the five pages already mapped have to go back - otherwise
 * every failed kmalloc on a nearly-full machine would consume the memory
 * it could not use, and the second failure would be cheaper than the
 * first only because the first had eaten everything. That is the shape of
 * bug that turns "out of memory" into "wedged".
 *
 * heap_virt_end is rewound too. The address space above it is untouched
 * and available again, which matters because these mappings are in the
 * kernel's own PML4 and would otherwise be permanently spoken for. */
static void *grow_heap(uint64_t pages) {
    uint64_t start = heap_virt_end;
    uint64_t mapped = 0;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_try_alloc_frame();
        if (!phys) {
            break;
        }
        if (vmm_try_map_page_in(vmm_kernel_pml4_phys(), heap_virt_end, phys,
                                VMM_FLAG_WRITABLE) != 0) {
            pmm_free_frame(phys);
            break;
        }
        heap_virt_end += PAGE_SIZE;
        mapped++;
    }
    if (mapped == pages) {
        return (void *)start;
    }
    for (uint64_t i = 0; i < mapped; i++) {
        heap_virt_end -= PAGE_SIZE;
        uint64_t phys = vmm_unmap_page_take(vmm_kernel_pml4_phys(), heap_virt_end);
        if (phys) {
            pmm_free_frame(phys);
        }
    }
    return (void *)0;
}

void heap_init(void) {
    heap_virt_end = vmm_kernel_heap_base();
    heap_head = (block_header_t *)0;
    klog_puts("[heap] kernel heap starts at 0x");
    klog_put_hex64(heap_virt_end);
    klog_putc('\n');
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return (void *)0;
    }
    size = (size_t)align_up(size, HEAP_ALIGN);

    uint64_t irq_flags = spin_lock_irqsave(&heap_lock);

    block_header_t *prev = (block_header_t *)0;
    for (block_header_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            /* Split off the remainder only if it's big enough to carry its
             * own header plus at least one aligned word - otherwise the
             * leftover sliver isn't worth tracking. */
            if (b->size >= size + sizeof(block_header_t) + HEAP_ALIGN) {
                block_header_t *rem = (block_header_t *)((uint8_t *)(b + 1) + size);
                rem->size = b->size - size - sizeof(block_header_t);
                rem->free = 1;
                rem->next = b->next;
                b->next = rem;
                b->size = size;
            }
            b->free = 0;
            spin_unlock_irqrestore(&heap_lock, irq_flags);
            return (void *)(b + 1);
        }
        prev = b;
    }

    /* Nothing free fits: grow the heap by enough whole pages for a new
     * block, and hand the whole thing out as one block (the unused tail,
     * if any, becomes the block's own trailing slack rather than a
     * separate free block - simpler, and the next kmalloc that needs more
     * room will just grow again). */
    size_t needed = sizeof(block_header_t) + size;
    uint64_t pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    block_header_t *b = (block_header_t *)grow_heap(pages);
    if (!b) {
        /* M102: NULL, and every caller of kmalloc has to look at it.
         * There were twenty-eight call sites when this became possible
         * and not one of them checked, because until now it could not
         * happen - which is exactly why turning the panic into a NULL
         * without auditing them all would have replaced one halt with
         * twenty-eight worse ones. */
        spin_unlock_irqrestore(&heap_lock, irq_flags);
        return (void *)0;
    }
    b->size = (size_t)(pages * PAGE_SIZE) - sizeof(block_header_t);
    b->free = 0;
    b->next = (block_header_t *)0;

    if (prev) {
        prev->next = b;
    } else {
        heap_head = b;
    }

    spin_unlock_irqrestore(&heap_lock, irq_flags);
    return (void *)(b + 1);
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }
    uint64_t irq_flags = spin_lock_irqsave(&heap_lock);
    block_header_t *b = (block_header_t *)ptr - 1;
    if (b->free) {
        panic("kfree: double free");
    }
    b->free = 1;

    /* Forward-only coalescing: the list is kept in address order (splits
     * insert adjacent, growth only ever appends at the high end), so the
     * immediate next node is always the next block by address - no need
     * to scan for or track a previous pointer to merge backward too. */
    while (b->next && b->next->free &&
           (uint8_t *)b->next == (uint8_t *)(b + 1) + b->size) {
        b->size += sizeof(block_header_t) + b->next->size;
        b->next = b->next->next;
    }
    spin_unlock_irqrestore(&heap_lock, irq_flags);
}
