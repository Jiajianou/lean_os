#include "syscall_wrappers.h"

#include <stdio.h>
#include <sys/mman.h>

#define CHUNK (64u * 1024 * 1024)

/* A ceiling, so a machine with far more memory than this expects stops
   rather than running until the harness gives up. It was 128 - 8 GiB - when
   a process had 128 mappings in all (M102) and no harness gave a guest more
   than 4 GiB. Since M156 the region table grows to 65536, and the ThinkPad's
   shape is 16 GiB: there [m102] reported "more memory than this test was
   built to exhaust" and panicked the battery. 1024 chunks is 64 GiB. */
#define MAX_CHUNKS 1024

int main(void) {
    unsigned long touched_bytes = 0;
    int chunks = 0;

    for (chunks = 0; chunks < MAX_CHUNKS; chunks++) {
        void *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("oomtest: mmap refused after %d chunks (%lu KiB touched) - "
                   "the reservation path reports failure\n",
                   chunks, touched_bytes / 1024);
            return 0;
        }

        volatile unsigned char *bytes = (volatile unsigned char *)p;
        for (unsigned int off = 0; off < CHUNK; off += 4096) {
            bytes[off] = 0xA5;
            touched_bytes += 4096;
        }
    }

    printf("oomtest: reserved and touched %d chunks (%lu KiB) without being "
           "refused or killed - this machine has more memory than this test "
           "was built to exhaust\n", chunks, touched_bytes / 1024);
    return 1;
}
