#include "syscall_wrappers.h"

#include <stdio.h>
#include <sys/mman.h>

#define CHUNK (64u * 1024 * 1024)

#define MAX_CHUNKS 128

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
