#include "malloc.h"

#include "syscall_wrappers.h"

#define HEAP_ALIGN 8UL   /* word alignment - matches kernel/mm/heap.c's own choice */
#define PAGE_SIZE  4096UL /* matches proc.h's PAGE_SIZE - sys_sbrk maps in whole pages */

typedef struct block_header {
    size_t size; /* usable bytes following this header - excludes the header itself */
    int free;
    struct block_header *next;
} block_header_t;

static block_header_t *heap_head;

static size_t align_up(size_t x, size_t a) {
    return (x + a - 1) & ~(a - 1);
}

/* Grows the process's heap by at least `min_bytes` via sys_sbrk, rounded
 * up to whole pages (sys_sbrk itself maps page by page - asking for
 * exactly what's needed, not more, keeps the kernel-side bookkeeping in
 * heap.c's spirit: grow by what's needed, nothing speculative). Returns
 * a pointer to the start of the new space, or NULL if sys_sbrk failed
 * (heap ceiling reached, out of physical memory, ...). */
static void *grow_heap(size_t min_bytes) {
    size_t pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    long prev_brk = sys_sbrk((long)(pages * PAGE_SIZE));
    if (prev_brk < 0) {
        return (void *)0;
    }
    return (void *)(unsigned long)prev_brk;
}

void *malloc(size_t size) {
    if (size == 0) {
        return (void *)0;
    }
    size = align_up(size, HEAP_ALIGN);

    block_header_t *prev = (block_header_t *)0;
    for (block_header_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            if (b->size >= size + sizeof(block_header_t) + HEAP_ALIGN) {
                block_header_t *rem = (block_header_t *)((unsigned char *)(b + 1) + size);
                rem->size = b->size - size - sizeof(block_header_t);
                rem->free = 1;
                rem->next = b->next;
                b->next = rem;
                b->size = size;
            }
            b->free = 0;
            return (void *)(b + 1);
        }
        prev = b;
    }

    size_t needed = sizeof(block_header_t) + size;
    block_header_t *b = (block_header_t *)grow_heap(needed);
    if (!b) {
        return (void *)0;
    }
    /* grow_heap rounds up to whole pages but only reports back the start
     * address - recompute exactly how much it actually granted from the
     * same rounding so the tail beyond `needed` becomes this block's own
     * slack, same as kernel/mm/heap.c's kmalloc does. */
    size_t pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    b->size = pages * PAGE_SIZE - sizeof(block_header_t);
    b->free = 0;
    b->next = (block_header_t *)0;

    if (prev) {
        prev->next = b;
    } else {
        heap_head = b;
    }

    return (void *)(b + 1);
}

void free(void *ptr) {
    if (!ptr) {
        return;
    }
    block_header_t *b = (block_header_t *)ptr - 1;
    if (b->free) {
        return; /* double free: no abort mechanism in user space yet, so this is a silent no-op rather than corrupting the free list */
    }
    b->free = 1;

    while (b->next && b->next->free &&
           (unsigned char *)b->next == (unsigned char *)(b + 1) + b->size) {
        b->size += sizeof(block_header_t) + b->next->size;
        b->next = b->next->next;
    }
}
