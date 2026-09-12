#include "malloc.h"

#include "mman.h" /* system_api/include/mman.h - PROT_/MAP_, M78 */
#include "syscall_wrappers.h"

/* ---- M114: sixteen, and it has to be sixteen -------------------------
 *
 * This was 8 ("word alignment - matches kernel/mm/heap.c's own choice")
 * from M19 until a browser fell over on a real web page.
 *
 * C requires malloc to return memory aligned for *any* type - on x86-64
 * that is `max_align_t`, which is 16, because the ABI's largest scalar
 * alignment is SSE's. The compiler is entitled to act on that, and it
 * does: GCC vectorises a struct copy or a memcpy into a malloc'd buffer
 * with `movaps`, which FAULTS rather than slows down on a misaligned
 * address.
 *
 * Every program on this machine had been getting 8 for forty-five
 * milestones and none of them noticed, because none of them was built
 * by a compiler that had vectorised a store into the heap. NetSurf is,
 * and the failure looked nothing like an allocator bug:
 *
 *   [isr] ring-3 fault: General protection fault in task netsurf
 *     rip=0x80001FCA56  code@rip = 0F 29 06   -> movaps %xmm0,(%rsi)
 *     rsi=0x9000B5AE68                        -> ...68, 8-aligned, not 16
 *
 * A browser that died partway through loading a large page, and the
 * three bytes at `rip` are the whole diagnosis. It is also why the
 * kernel prints them: see kernel/arch/x86_64/isr.c.
 *
 * The header is 32 bytes and grow_heap takes whole pages from sbrk, so
 * rounding every request up to 16 is sufficient - a payload sits at a
 * 16-aligned base plus 32, and a split remainder sits that many aligned
 * bytes further on. Graded by tests/test_malloc.c, which now asserts the
 * alignment of every pointer it gets rather than only that it got one. */
#define HEAP_ALIGN 16UL
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
    /* M98: `is_free` rather than `free`. The member had the same name as
     * the function since M19, which was harmless until this file started
     * being compiled a second time for the host tests - where the test
     * renames the FUNCTIONS to keep them out of the host libc's way, and
     * a macro cannot tell a member from a call. */
    int is_free;
    struct block_header *next;
    /* M78: this block is its own mmap mapping rather than a slice of the
     * sbrk heap, so free() unmaps it instead of putting it on the list.
     * A flag in the header rather than a separate table of large
     * pointers: free() already has the header in its hand, and a second
     * structure to keep in step is a second structure to get wrong. */
    int mmapped;
} block_header_t;

/* ---- M98: the free list stops being the block list --------------------
 *
 * **Measured, not guessed.** M98 profiled a C++ compile on this machine
 * with M101's sampler and 72% of every non-idle sample landed on two
 * adjacent instructions inside this file: the first-fit loop, which
 * walked `heap_head` - a list of EVERY block, free or not, in address
 * order. GCC makes hundreds of thousands of small allocations, so that
 * list grows to hundreds of thousands of nodes and every `malloc` walks
 * it from the front. The allocator was quadratic in the number of live
 * allocations, and a compile that costs 0.25 s of C on the host cost
 * 116x that here while the same C++ compile cost 460x - the gap between
 * those two numbers is this loop.
 *
 * Two changes, and the second is what makes the first bounded:
 *
 * **Free blocks are on their own list**, threaded through the block's
 * own payload rather than through a bigger header - a free block's bytes
 * belong to nobody, which is where every real allocator puts these
 * links. The address-ordered `next` chain stays exactly as it was,
 * because that is what free() coalesces along.
 *
 * **The free list is split by size class**, so a request for 32 bytes
 * looks at blocks that might fit rather than at every free block on the
 * machine. Bin i holds blocks whose usable size is in [2^(i+4),
 * 2^(i+5)) - the first bin starts at 16 bytes because that is the
 * smallest block that can hold the two links.
 *
 * What this is NOT: it is not a best-fit, it does not coalesce
 * backwards, and it has no per-thread arenas. Each of those is a real
 * allocator's answer to a measurement nobody here has taken. This one
 * answers the measurement that WAS taken and stops there. */
#define MIN_PAYLOAD (2 * sizeof(void *)) /* two links live in a free block's bytes */
#define NBINS 28                          /* 16 B up to 2 GiB, which is the arena */

typedef struct free_links {
    struct block_header *fnext;
    struct block_header *fprev;
} free_links_t;

#define LINKS(b) ((free_links_t *)((b) + 1))

static block_header_t *heap_head;
/* The last block in address order, so grow_heap can append without
 * walking. Before M98 the walk that found it was the same walk that
 * found a free block, and both were the bug. */
static block_header_t *heap_tail;
static block_header_t *bins[NBINS];

/* The bin a block of `size` usable bytes belongs in: the index of its
 * highest set bit, minus four, clamped. Deliberately a loop rather than
 * a bit-scan builtin - this is a handful of iterations on a path that
 * has just done a syscall's worth of work, and the builtin's name
 * differs between the two compilers this file is built with. */
static int bin_of(size_t size) {
    int i = 0;
    size_t s = size >> 4;
    while (s > 1 && i < NBINS - 1) {
        s >>= 1;
        i++;
    }
    return i;
}

static void bin_insert(block_header_t *b) {
    int i = bin_of(b->size);
    LINKS(b)->fnext = bins[i];
    LINKS(b)->fprev = (block_header_t *)0;
    if (bins[i]) {
        LINKS(bins[i])->fprev = b;
    }
    bins[i] = b;
}

static void bin_remove(block_header_t *b) {
    int i = bin_of(b->size);
    block_header_t *n = LINKS(b)->fnext;
    block_header_t *p = LINKS(b)->fprev;
    if (p) {
        LINKS(p)->fnext = n;
    } else if (bins[i] == b) {
        bins[i] = n;
    }
    if (n) {
        LINKS(n)->fprev = p;
    }
    LINKS(b)->fnext = (block_header_t *)0;
    LINKS(b)->fprev = (block_header_t *)0;
}

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

/* M98: two spellings of one instruction, because this file is now
 * compiled twice - once for this machine and once for the host, where
 * tests/test_malloc.c grades the allocator itself. The x86 form is the
 * one that ships and is unchanged; the builtin is what the host
 * compiler has, and a test binary's lock does not have to be the
 * machine's lock, it has to be A lock. */
static inline int heap_xchg(volatile int *p, int v) {
#if defined(__x86_64__)
    __asm__ volatile("lock xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
    return v;
#else
    return __sync_lock_test_and_set(p, v);
#endif
}

static void heap_acquire(void) {
    for (;;) {
        for (int spin = 0; spin < 200; spin++) {
            if (heap_xchg(&heap_lock, 1) == 0) {
                return;
            }
#if defined(__x86_64__)
            __asm__ volatile("pause" ::: "memory");
#else
            __asm__ volatile("" ::: "memory");
#endif
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
    /* M117: malloc(0) returns a unique, freeable block and NOT NULL. C
     * permits either, and this returned NULL from M19 until NetSurf
     * asked for the size of an empty flex container's item list:
     * layout_flex.c does `calloc(count, sizeof item)` with count 0, and
     * the NULL it got back reads, in every program written against
     * glibc, macOS or musl - all of which hand back a pointer here - as
     * out of memory. NetSurf's layout failed silently on the first empty
     * `display: flex` box on the page and died on its own assertion
     * some frames later, on apple.com and on Wikipedia, while the same
     * NetSurf built for the host rendered both. It took the port's
     * first backtrace and a bisection of apple.com down to three empty
     * divs to find, because the refusal was legal and nobody looked.
     *
     * The minimum block, so the pointer is real, distinct from every
     * other live allocation, and free() takes it back. */
    if (size == 0) {
        size = 1;
    }
    size = align_up(size, HEAP_ALIGN);
    /* M98: never smaller than the two links a free block carries in its
     * own payload. A 4-byte allocation that is later freed still has to
     * be able to sit on a free list. */
    if (size < MIN_PAYLOAD) {
        size = MIN_PAYLOAD;
    }

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
            m->is_free = 0;
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
    /* The bin this size belongs in, then every larger bin. A block in a
     * larger bin always fits; a block in this one might not, because a
     * bin is a size RANGE - so the first list is searched and the rest
     * are taken from the front. That is first-fit within a size class,
     * which is close enough to best-fit to keep fragmentation ordinary
     * and is O(1) in the common case. */
    for (int i = bin_of(size); i < NBINS; i++) {
        for (block_header_t *b = bins[i]; b; b = LINKS(b)->fnext) {
            if (b->size < size) {
                continue;
            }
            bin_remove(b);
            /* Split only when the remainder can itself be a block that
             * holds the two links - a smaller tail would be a free
             * block that cannot go on a free list. */
            if (b->size >= size + sizeof(block_header_t) + MIN_PAYLOAD) {
                block_header_t *rem = (block_header_t *)((unsigned char *)(b + 1) + size);
                rem->size = b->size - size - sizeof(block_header_t);
                rem->is_free = 1;
                rem->mmapped = 0;
                rem->next = b->next;
                b->next = rem;
                if (heap_tail == b) {
                    heap_tail = rem;
                }
                b->size = size;
                bin_insert(rem);
            }
            b->is_free = 0;
            heap_release();
            return (void *)(b + 1);
        }
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
    b->is_free = 0;
    b->next = (block_header_t *)0;
    b->mmapped = 0;

    /* Appended at the tail rather than at whatever the search stopped
     * on: sbrk only ever grows upward, so the newest block is always
     * the highest address, and that is what keeps `next` in address
     * order - which is the invariant free()'s coalesce depends on. */
    if (heap_tail) {
        heap_tail->next = b;
    } else {
        heap_head = b;
    }
    heap_tail = b;

    heap_release();
    return (void *)(b + 1);
}

void free(void *ptr) {
    if (!ptr) {
        return;
    }
    block_header_t *b = (block_header_t *)ptr - 1;
    if (b->is_free) {
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
    b->is_free = 1;

    /* Coalesce forward, exactly as before - and now each absorbed block
     * has to come OFF its free list first, because a merged block's
     * bytes are about to become somebody's payload. Forgetting that is
     * the classic version of this bug: the bin keeps a pointer into the
     * middle of a live allocation and hands it out later. */
    while (b->next && b->next->is_free &&
           (unsigned char *)b->next == (unsigned char *)(b + 1) + b->size) {
        block_header_t *victim = b->next;
        bin_remove(victim);
        if (heap_tail == victim) {
            heap_tail = b;
        }
        b->size += sizeof(block_header_t) + victim->size;
        b->next = victim->next;
    }
    bin_insert(b);
    heap_release();
}

size_t malloc_usable_size(void *ptr) {
    if (!ptr) {
        return 0;
    }
    return ((block_header_t *)ptr - 1)->size;
}
