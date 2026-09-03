/* kernel/mm/heap.h
 *
 * Kernel heap: a first-fit free-list allocator (kmalloc/kfree) over a
 * virtual range starting just above the 1 GiB identity map vmm_init()
 * builds (heap.c), grown page by page via vmm_map_page/pmm_alloc_frame as
 * allocations need more space than is already mapped.
 */
#pragma once

#include <stddef.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

/* Q9: bytes in live blocks, and bytes this allocator has ever taken from
 * the VMM. A leak grows the first; fragmentation grows the second while
 * the first returns to where it started, and telling those two apart is
 * the whole reason there are two numbers. See heap.c. */
size_t heap_used_bytes(void);
size_t heap_total_bytes(void);
