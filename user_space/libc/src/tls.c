#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "syscall_wrappers.h"
#include "process.h"

extern char __lean_tls_init_start[] __attribute__((weak));
extern char __lean_tls_init_end[] __attribute__((weak));
extern char __lean_tls_end[] __attribute__((weak));
extern char __lean_tls_align[] __attribute__((weak));

static int thread_pointer_ready;

size_t __lean_tls_init_size(void) {
    if (!__lean_tls_init_start || !__lean_tls_init_end) {
        return 0;
    }
    return (size_t)(__lean_tls_init_end - __lean_tls_init_start);
}

size_t __lean_tls_total_size(void) {
    if (!__lean_tls_init_start || !__lean_tls_end) {
        return 0;
    }
    return (size_t)(__lean_tls_end - __lean_tls_init_start);
}

/* An absolute symbol the linker script assigns, so its ADDRESS is the number.
   A program linked before that symbol existed has none, and one alignment
   byte is the answer that changes nothing. */
size_t __lean_tls_alignment(void) {
    size_t value = (size_t)(unsigned long)__lean_tls_align;
    if (value < 1 || (value & (value - 1)) != 0) {
        return 1;
    }
    return value;
}

/* The block's size, the same every time for a given program, so the release
   below can be handed a pointer and nothing else. */
static size_t tls_block_bytes(void) {
    size_t align = __lean_tls_alignment();
    size_t total = __lean_tls_total_size();

    /* The size the COMPILER uses, which is the segment rounded up to the
       segment's own alignment - not the segment's size. The two are the same
       number for most programs and that is why this was wrong for sixty-odd
       milestones without anybody noticing: it took a thread_local with a
       vtable in a segment of 0x134 bytes aligned to 8 for the four bytes of
       difference to become a jump through a pointer read four bytes low. */
    size_t span = (total + align - 1u) & ~(align - 1u);

    /* Whatever the segment asks for, and never less than a cache line, which
       is what this held before and what keeps one thread's block off the end
       of another's. */
    size_t pointer_align = align > 64u ? align : 64u;
    return span + pointer_align + 64u;
}

/* The block comes from mmap and not from malloc, and M187 is why. This runs
   on a new thread BEFORE its thread pointer exists - that is what it is for -
   and a program is entitled to replace malloc. Chromium does: its allocator
   shim puts PartitionAlloc behind malloc and GWP-ASan in front of it, and
   GWP-ASan's first act is to read a thread_local. On a thread whose FS base
   was still zero that read was at address zero, and the thread died in
   gwp_asan::AllocFn before its start routine ran. A system call is the one
   allocator nothing can interpose, and its pages arrive zeroed. */
void *__lean_tls_setup(void) {
    unsigned long existing = 0;
    if (sys_arch_prctl(ARCH_GET_FS, (unsigned long)&existing) == 0 &&
        existing != 0) {
        return 0;
    }

    size_t align = __lean_tls_alignment();
    size_t total = __lean_tls_total_size();
    size_t init = __lean_tls_init_size();
    size_t span = (total + align - 1u) & ~(align - 1u);
    size_t pointer_align = align > 64u ? align : 64u;
    size_t bytes = tls_block_bytes();

    void *mapped = mmap(0, bytes, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mapped == MAP_FAILED) {
        return 0;
    }
    char *block = (char *)mapped;

    /* tp is aligned, and span is a multiple of the segment alignment, so
       tp - span is aligned too - which is what every variable's offset from
       tp was computed against. */
    char *tp = block + span;
    tp = (char *)(((unsigned long)tp + pointer_align - 1u) &
                  ~(unsigned long)(pointer_align - 1u));
    if (init > 0) {
        memcpy(tp - span, __lean_tls_init_start, init);
    }
    *(void **)tp = tp;

    if (sys_arch_prctl(ARCH_SET_FS, (unsigned long)tp) != 0) {
        munmap(block, bytes);
        return 0;
    }
    thread_pointer_ready = 1;
    return block;
}

/* M202. pthread_self() and gettid() were a system call each, and Chromium
   asks for one or the other on almost every lock it takes and every task it
   posts: under QEMU the browser made 300,000 of them a second, and on the
   laptop - where its own code runs thirty times faster and the kernel entry
   does not - that was most of what three processors spent eighty-five
   percent of their time in the kernel doing. A thread's id does not change,
   so the first answer is kept in the word after the thread pointer, which
   tls_block_bytes leaves room for and nothing else uses; a forked child is
   a new thread with the parent's memory and forgets it. A process whose
   thread pointer was never set up asks the kernel every time, as before. */
#define THREAD_ID_OFFSET 8

long __lean_thread_id(void) {
    if (!thread_pointer_ready) {
        return sys_gettid();
    }
    long id;
    __asm__ volatile("movq %%fs:%c1, %0" : "=r"(id) : "i"(THREAD_ID_OFFSET));
    if (id == 0) {
        id = sys_gettid();
        __asm__ volatile("movq %0, %%fs:%c1" : : "r"(id), "i"(THREAD_ID_OFFSET) : "memory");
    }
    return id;
}

void __lean_forget_thread_id(void) {
    if (thread_pointer_ready) {
        __asm__ volatile("movq $0, %%fs:%c0" : : "i"(THREAD_ID_OFFSET) : "memory");
    }
}

void __lean_tls_release(void *block) {
    if (block) {
        munmap(block, tls_block_bytes());
    }
}
