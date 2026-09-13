#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL

#define HUGE_PAGES (36864UL)

#define TOUCH_PAGES 256UL

#define PARK_YIELDS 600

extern long sys_yield(void);

static int ordinary(void) {
    unsigned char *huge = (unsigned char *)mmap(0, HUGE_PAGES * PAGE,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (huge == MAP_FAILED) {
        return 2;
    }

    unsigned long stride = HUGE_PAGES / TOUCH_PAGES;
    for (unsigned long i = 0; i < TOUCH_PAGES; i++) {
        huge[i * stride * PAGE] = (unsigned char)(i + 1);
    }
    for (unsigned long i = 0; i < TOUCH_PAGES; i++) {
        if (huge[i * stride * PAGE] != (unsigned char)(i + 1)) {
            return 3;
        }
    }

    if (huge[(HUGE_PAGES / 2) * PAGE + 7] != 0) {
        return 4;
    }

    printf("lazytest: %lu MiB reserved, %lu pages touched - parking\n",
           (HUGE_PAGES * PAGE) / (1024UL * 1024UL), TOUCH_PAGES);
    for (int i = 0; i < PARK_YIELDS; i++) {
        sys_yield();
    }

    if (munmap(huge, HUGE_PAGES * PAGE) != 0) {
        return 5;
    }

    for (int round = 0; round < 8; round++) {
        unsigned char *p = (unsigned char *)mmap(0, HUGE_PAGES * PAGE,
                                                  PROT_READ | PROT_WRITE,
                                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) {
            return 5;
        }
        p[0] = (unsigned char)round;
        p[(HUGE_PAGES - 1) * PAGE] = (unsigned char)round;
        if (p[0] != (unsigned char)round || p[(HUGE_PAGES - 1) * PAGE] != (unsigned char)round) {
            return 3;
        }
        if (munmap(p, HUGE_PAGES * PAGE) != 0) {
            return 5;
        }
    }

    char *buffer = (char *)mmap(0, 16 * PAGE, PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (buffer == MAP_FAILED) {
        return 2;
    }
    if (getcwd(buffer, 16 * PAGE) == 0 || buffer[0] != '/') {
        printf("lazytest: an untouched mmap buffer was refused as a syscall argument\n");
        return 7;
    }
    munmap(buffer, 16 * PAGE);

    unsigned char *ro = (unsigned char *)mmap(0, 4 * PAGE, PROT_READ,
                                               MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (ro == MAP_FAILED) {
        return 2;
    }
    for (int i = 0; i < 4; i++) {
        if (ro[i * (long)PAGE] != 0) {
            return 6;
        }
    }
    munmap(ro, 4 * PAGE);

    printf("lazytest: all checks passed\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && strcmp(argv[1], "ro") == 0) {
        volatile unsigned char *ro = (volatile unsigned char *)mmap(
            0, PAGE, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (ro == (volatile unsigned char *)MAP_FAILED) {
            return 2;
        }
        ro[0] = 1;
        printf("lazytest: writing to a PROT_READ mapping was allowed\n");
        return 1;
    }

    if (argc > 1 && argv[1] && strcmp(argv[1], "gap") == 0) {
        volatile unsigned char *p = (volatile unsigned char *)mmap(
            0, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == (volatile unsigned char *)MAP_FAILED) {
            return 2;
        }
        p[64UL * 1024UL * 1024UL] = 1;
        printf("lazytest: touching unreserved arena address space was allowed\n");
        return 1;
    }

    return ordinary();
}
