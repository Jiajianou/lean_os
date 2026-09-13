#include <stdlib.h>
#include <string.h>

#include "syscall_wrappers.h"
#include "process.h"

extern char __lean_tls_init_start[] __attribute__((weak));
extern char __lean_tls_init_end[] __attribute__((weak));
extern char __lean_tls_end[] __attribute__((weak));

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

void *__lean_tls_setup(void) {
    unsigned long existing = 0;
    if (sys_arch_prctl(ARCH_GET_FS, (unsigned long)&existing) == 0 &&
        existing != 0) {
        return 0;
    }

    size_t total = __lean_tls_total_size();
    size_t init = __lean_tls_init_size();
    size_t aligned_total = (total + 63u) & ~(size_t)63u;
    size_t bytes = aligned_total + 64;

    char *block = (char *)malloc(bytes);
    if (!block) {
        return 0;
    }
    memset(block, 0, bytes);
    char *tp = block + aligned_total;
    if (init > 0) {
        memcpy(tp - total, __lean_tls_init_start, init);
    }
    *(void **)tp = tp;

    if (sys_arch_prctl(ARCH_SET_FS, (unsigned long)tp) != 0) {
        free(block);
        return 0;
    }
    return block;
}
