#include <stdlib.h>
#include <string.h>

#include "syscall_wrappers.h"
#include "process.h"

extern char __lean_tls_init_start[] __attribute__((weak));
extern char __lean_tls_init_end[] __attribute__((weak));
extern char __lean_tls_end[] __attribute__((weak));
extern char __lean_tls_align[] __attribute__((weak));

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

void *__lean_tls_setup(void) {
    unsigned long existing = 0;
    if (sys_arch_prctl(ARCH_GET_FS, (unsigned long)&existing) == 0 &&
        existing != 0) {
        return 0;
    }

    size_t align = __lean_tls_alignment();
    size_t total = __lean_tls_total_size();
    size_t init = __lean_tls_init_size();

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
    size_t bytes = span + pointer_align + 64u;

    char *block = (char *)malloc(bytes);
    if (!block) {
        return 0;
    }
    memset(block, 0, bytes);

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
        free(block);
        return 0;
    }
    return block;
}
