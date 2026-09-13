#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fakes.h"

#define FAKE_HEAP_BYTES (64u * 1024u * 1024u)
#define FAKE_PAGE 4096u

static unsigned char *heap_base;
static size_t heap_used;
static size_t heap_cap;
static int sbrk_refusing;

void fake_user_heap_reset(void) {
    if (!heap_base) {
        heap_base = (unsigned char *)mmap(NULL, FAKE_HEAP_BYTES,
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    heap_used = 0;
    heap_cap = FAKE_HEAP_BYTES;
    sbrk_refusing = 0;
}

void fake_user_sbrk_refuse(int on) { sbrk_refusing = on; }

size_t fake_user_heap_used(void) { return heap_used; }

long sys_sbrk(long increment) {
    if (sbrk_refusing || increment < 0) {
        return -1;
    }
    size_t want = (size_t)increment;
    if (want % FAKE_PAGE) {
        want += FAKE_PAGE - (want % FAKE_PAGE);
    }
    if (heap_used + want > heap_cap) {
        return -1;
    }
    unsigned char *p = heap_base + heap_used;
    heap_used += want;
    memset(p, 0, want);
    return (long)(uintptr_t)p;
}

long sys_mmap(long addr, unsigned long len, int prot, int flags, int fd,
              unsigned long offset) {
    (void)addr;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return -1;
    }
    return (long)(uintptr_t)p;
}

long sys_munmap(void *addr, unsigned long len) {
    return munmap(addr, len) == 0 ? 0 : -1;
}

long sys_yield(void) { return 0; }
