#include "malloc.h"

#include "mman.h"
#include "syscall_wrappers.h"

#define HEAP_ALIGN 16UL
#define PAGE_SIZE  4096UL

#define MMAP_THRESHOLD (64UL * 1024UL)

typedef struct block_header {
    size_t size;
    int is_free;
    struct block_header *next;
    int mmapped;
} block_header_t;

#define MIN_PAYLOAD (2 * sizeof(void *))
#define NBINS 28

typedef struct free_links {
    struct block_header *fnext;
    struct block_header *fprev;
} free_links_t;

#define LINKS(b) ((free_links_t *)((b) + 1))

static block_header_t *heap_head;
static block_header_t *heap_tail;
static block_header_t *bins[NBINS];

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

static volatile int heap_lock;

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

static void *grow_heap(size_t min_bytes) {
    size_t pages = (min_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    long previous_brk = sys_sbrk((long)(pages * PAGE_SIZE));
    if (previous_brk < 0) {
        return (void *)0;
    }
    return (void *)(unsigned long)previous_brk;
}

void *malloc(size_t size) {
    if (size == 0) {
        size = 1;
    }
    size = align_up(size, HEAP_ALIGN);
    if (size < MIN_PAYLOAD) {
        size = MIN_PAYLOAD;
    }

    if (size >= MMAP_THRESHOLD) {
        size_t total = align_up(sizeof(block_header_t) + size, PAGE_SIZE);
        long addr = sys_mmap(0, total, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (addr >= 0) {
            block_header_t *m = (block_header_t *)(unsigned long)addr;
            m->size = total - sizeof(block_header_t);
            m->is_free = 0;
            m->next = (block_header_t *)0;
            m->mmapped = 1;
            return (void *)(m + 1);
        }
    }

    heap_acquire();
    for (int i = bin_of(size); i < NBINS; i++) {
        for (block_header_t *b = bins[i]; b; b = LINKS(b)->fnext) {
            if (b->size < size) {
                continue;
            }
            bin_remove(b);
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
    size_t pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    b->size = pages * PAGE_SIZE - sizeof(block_header_t);
    b->is_free = 0;
    b->next = (block_header_t *)0;
    b->mmapped = 0;

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
        return;
    }
    if (b->mmapped) {
        sys_munmap(b, b->size + sizeof(block_header_t));
        return;
    }
    heap_acquire();
    b->is_free = 1;

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
