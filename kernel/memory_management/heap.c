#include "heap.h"

#include <stddef.h>
#include <stdint.h>

#include "drivers/kernel_log.h"
#include "library/spinlock.h"
#include "memory_management/physical_memory.h"
#include "memory_management/virtual_memory.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL
#define HEAP_ALIGN 8ULL

typedef struct block_header {
    size_t size;
    int free;
    struct block_header *next;
} block_header_t;

static uint64_t heap_virt_end;
static block_header_t *heap_head;

static spinlock_t heap_lock;

static uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

static void *grow_heap(uint64_t pages) {
    uint64_t start = heap_virt_end;
    uint64_t mapped = 0;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = physical_memory_try_alloc_frame();
        if (!phys) {
            break;
        }
        if (virtual_memory_try_map_page_in(virtual_memory_kernel_pml4_phys(), heap_virt_end, phys,
                                VIRTUAL_MEMORY_FLAG_WRITABLE) != 0) {
            physical_memory_free_frame(phys);
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
        uint64_t phys = virtual_memory_unmap_page_take(virtual_memory_kernel_pml4_phys(), heap_virt_end);
        if (phys) {
            physical_memory_free_frame(phys);
        }
    }
    return (void *)0;
}

static size_t heap_used;
static size_t heap_total;

size_t heap_used_bytes(void) {
    uint64_t f = spin_lock_irqsave(&heap_lock);
    size_t n = heap_used;
    spin_unlock_irqrestore(&heap_lock, f);
    return n;
}

size_t heap_total_bytes(void) {
    uint64_t f = spin_lock_irqsave(&heap_lock);
    size_t n = heap_total;
    spin_unlock_irqrestore(&heap_lock, f);
    return n;
}

void heap_init(void) {
    heap_virt_end = virtual_memory_kernel_heap_base();
    heap_head = (block_header_t *)0;
    heap_used = 0;
    heap_total = 0;
    kernel_log_puts("[heap] kernel heap starts at 0x");
    kernel_log_put_hex64(heap_virt_end);
    kernel_log_putc('\n');
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return (void *)0;
    }
    size = (size_t)align_up(size, HEAP_ALIGN);

    uint64_t irq_flags = spin_lock_irqsave(&heap_lock);

    block_header_t *previous = (block_header_t *)0;
    for (block_header_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            if (b->size >= size + sizeof(block_header_t) + HEAP_ALIGN) {
                block_header_t *rem = (block_header_t *)((uint8_t *)(b + 1) + size);
                rem->size = b->size - size - sizeof(block_header_t);
                rem->free = 1;
                rem->next = b->next;
                b->next = rem;
                b->size = size;
            }
            b->free = 0;
            heap_used += b->size;
            spin_unlock_irqrestore(&heap_lock, irq_flags);
            return (void *)(b + 1);
        }
        previous = b;
    }

    size_t needed = sizeof(block_header_t) + size;
    uint64_t pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    block_header_t *b = (block_header_t *)grow_heap(pages);
    if (!b) {
        spin_unlock_irqrestore(&heap_lock, irq_flags);
        return (void *)0;
    }
    b->size = (size_t)(pages * PAGE_SIZE) - sizeof(block_header_t);
    b->free = 0;
    b->next = (block_header_t *)0;
    heap_used += b->size;
    heap_total += (size_t)(pages * PAGE_SIZE);

    if (previous) {
        previous->next = b;
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
    heap_used -= b->size;

    while (b->next && b->next->free &&
           (uint8_t *)b->next == (uint8_t *)(b + 1) + b->size) {
        b->size += sizeof(block_header_t) + b->next->size;
        b->next = b->next->next;
    }
    spin_unlock_irqrestore(&heap_lock, irq_flags);
}
