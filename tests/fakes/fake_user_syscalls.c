/* tests/fakes/fake_user_syscalls.c - M98
 *
 * The four calls user_space/lib/malloc.c makes, backed by host memory,
 * so the allocator itself can be compiled for the machine you are
 * sitting at and tested in microseconds.
 *
 * Why this fake exists at all: M98 profiled a C++ compile on the machine
 * and found 72% of it inside malloc's free-list walk. The fix - size
 * binned free lists threaded through free blocks' own payloads - is the
 * kind of change that is either right or corrupts memory, and "corrupts
 * memory" on a machine reached only through a five-minute boot is the
 * worst debugging position this project has. So the allocator joins the
 * scheduler, the heap, leanfs and the network parsers on the host tier.
 *
 * `sbrk` is a real bump allocator over one big host mapping, which is
 * what the kernel's own sbrk is: address space that only grows, handed
 * out in whole pages. `mmap` and `munmap` are host mmap and munmap,
 * because they are what they claim to be.
 */
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

/* The four wrappers malloc.c calls. Names match
 * user_space/lib/syscall_wrappers.h exactly - that header is not
 * included here, because including it drags in the whole ABI. */
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
