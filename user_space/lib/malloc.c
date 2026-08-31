#include "malloc.h"

#include "mman.h" /* system_api/include/mman.h - PROT_/MAP_, M78 */
#include "syscall_wrappers.h"

#define HEAP_ALIGN 8UL   /* word alignment - matches kernel/mm/heap.c's own choice */
#define PAGE_SIZE  4096UL /* matches proc.h's PAGE_SIZE - sys_sbrk maps in whole pages */

/* ---- M78: the size-based split every real allocator makes -------------
 *
 * Above this, an allocation gets its own SYS_mmap mapping and its own
 * SYS_munmap on free; below it, it comes out of the sbrk heap and its
 * free list exactly as it has since M19.
 *
 * The reason for the split is not speed, it is that sbrk cannot shrink.
 * A 4 MiB buffer allocated and freed out of a bump-allocated heap leaves
 * 4 MiB this process holds until it exits - the free list can hand those
 * bytes out again, but only to this process and only for this process's
 * lifetime. A large allocation is exactly the case where that matters
 * and exactly the case where one extra syscall is not worth measuring
 * against the work the caller is about to do with the memory.
 *
 * 64 KiB, which is sixteen pages: large enough that the per-mapping cost
 * (a syscall, a kernel table entry, and a whole page of rounding at the
 * tail) is noise, small enough that the allocations a program makes
 * *once* - a file buffer, an image, an arena - are on the mmap side of
 * the line and the ones it makes in a loop are not. */
#define MMAP_THRESHOLD (64UL * 1024UL)

typedef struct block_header {
    size_t size; /* usable bytes following this header - excludes the header itself */
    int free;
    struct block_header *next;
    /* M78: this block is its own mmap mapping rather than a slice of the
     * sbrk heap, so free() unmaps it instead of putting it on the list.
     * A flag in the header rather than a separate table of large
     * pointers: free() already has the header in its hand, and a second
     * structure to keep in step is a second structure to get wrong. */
    int mmapped;
} block_header_t;

static block_header_t *heap_head;

/* ---- M79: one free list, two threads ----------------------------------
 *
 * This allocator was written when a process had exactly one thread of
 * control, and every line of it assumes that: the free-list walk, the
 * split, the coalesce in free(). Two threads in one address space share
 * this list, and two of them inside it at once produce a list that is
 * quietly wrong - a block on it twice, or a `next` pointing at a header
 * that has been split out from under it.
 *
 * So it takes a lock. Deliberately the same spin-then-yield shape
 * <pthread.h>'s mutex uses rather than that mutex itself: malloc is
 * linked into every program on this machine and most of them will never
 * have a second thread, so it cannot depend on the thread library. What
 * it needs is one atomic instruction, and that is what this is.
 *
 * Uncontended, this costs one `lock xchg` per malloc/free - which is the
 * price every real allocator pays for the same reason, and is invisible
 * next to the syscall a growing heap makes anyway.
 */
static volatile int heap_lock;

static inline int heap_xchg(volatile int *p, int v) {
    __asm__ volatile("lock xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
    return v;
}

static void heap_acquire(void) {
    for (;;) {
        for (int spin = 0; spin < 200; spin++) {
            if (heap_xchg(&heap_lock, 1) == 0) {
                return;
            }
            __asm__ volatile("pause" ::: "memory");
        }
        sys_yield();
    }
}

static void heap_release(void) {
    heap_xchg(&heap_lock, 0);
}

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

    /* M78: large allocations get their own mapping, so that freeing one
     * actually returns the pages. See MMAP_THRESHOLD above. */
    if (size >= MMAP_THRESHOLD) {
        size_t total = align_up(sizeof(block_header_t) + size, PAGE_SIZE);
        long addr = sys_mmap(0, total, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (addr >= 0) {
            block_header_t *m = (block_header_t *)(unsigned long)addr;
            /* The whole rounded mapping is this block's, tail included -
             * the same "the slack is yours" rule the sbrk path below
             * follows, and what makes malloc_usable_size honest. */
            m->size = total - sizeof(block_header_t);
            m->free = 0;
            m->next = (block_header_t *)0;
            m->mmapped = 1;
            return (void *)(m + 1);
        }
        /* No mapping to be had - fall through to the sbrk heap rather
         * than failing. The arena can be full while the heap is not, and
         * a malloc that failed for a reason the caller cannot see or act
         * on is worse than a slower one that works. */
    }

    heap_acquire();
    block_header_t *prev = (block_header_t *)0;
    for (block_header_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            if (b->size >= size + sizeof(block_header_t) + HEAP_ALIGN) {
                block_header_t *rem = (block_header_t *)((unsigned char *)(b + 1) + size);
                rem->size = b->size - size - sizeof(block_header_t);
                rem->free = 1;
                rem->mmapped = 0;
                rem->next = b->next;
                b->next = rem;
                b->size = size;
            }
            b->free = 0;
            heap_release();
            return (void *)(b + 1);
        }
        prev = b;
    }

    size_t needed = sizeof(block_header_t) + size;
    block_header_t *b = (block_header_t *)grow_heap(needed);
    if (!b) {
        heap_release();
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
    b->mmapped = 0;

    if (prev) {
        prev->next = b;
    } else {
        heap_head = b;
    }

    heap_release();
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
    /* M78: a large allocation goes back to the kernel rather than onto
     * the free list. It was never on the list, so there is nothing here
     * to unlink - which is the whole benefit of giving each one its own
     * mapping rather than carving it out of the heap. */
    if (b->mmapped) {
        sys_munmap(b, b->size + sizeof(block_header_t));
        return;
    }
    heap_acquire();
    b->free = 1;

    while (b->next && b->next->free &&
           (unsigned char *)b->next == (unsigned char *)(b + 1) + b->size) {
        b->size += sizeof(block_header_t) + b->next->size;
        b->next = b->next->next;
    }
    heap_release();
}

size_t malloc_usable_size(void *ptr) {
    if (!ptr) {
        return 0;
    }
    return ((block_header_t *)ptr - 1)->size;
}
