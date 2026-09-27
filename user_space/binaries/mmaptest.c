#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL

static int touch(unsigned char *p, unsigned long pages, unsigned char v) {
    for (unsigned long i = 0; i < pages; i++) {
        p[i * PAGE] = v;
        p[i * PAGE + PAGE - 1] = v;
    }
    for (unsigned long i = 0; i < pages; i++) {
        if (p[i * PAGE] != v || p[i * PAGE + PAGE - 1] != v) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    unsigned char *big = (unsigned char *)mmap(0, 64 * PAGE, PROT_READ | PROT_WRITE,
                                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (big == MAP_FAILED) {
        return 2;
    }
    for (unsigned long i = 0; i < 64; i++) {
        if (big[i * PAGE] != 0 || big[i * PAGE + PAGE - 1] != 0) {
            return 4;
        }
    }
    if (!touch(big, 64, 0xA5)) {
        return 3;
    }

    if (munmap(big, 32 * PAGE) != 0) {
        return 2;
    }
    unsigned char *reused = (unsigned char *)mmap(0, 32 * PAGE, PROT_READ | PROT_WRITE,
                                                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (reused == MAP_FAILED) {
        return 2;
    }
    if (reused != big) {
        printf("mmaptest: the freed range was not reused (%p vs %p)\n",
                (void *)reused, (void *)big);
        return 5;
    }
    for (unsigned long i = 0; i < 32; i++) {
        if (reused[i * PAGE] != 0) {
            return 4;
        }
    }
    if (!touch(reused, 32, 0x5A)) {
        return 3;
    }
    for (unsigned long i = 32; i < 64; i++) {
        if (big[i * PAGE] != 0xA5) {
            return 3;
        }
    }

    unsigned char *a = (unsigned char *)mmap(0, 4 * PAGE, PROT_READ | PROT_WRITE,
                                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    unsigned char *b = (unsigned char *)mmap(0, 4 * PAGE, PROT_READ | PROT_WRITE,
                                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    unsigned char *c = (unsigned char *)mmap(0, 4 * PAGE, PROT_READ | PROT_WRITE,
                                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (a == MAP_FAILED || b == MAP_FAILED || c == MAP_FAILED) {
        return 2;
    }
    if (munmap(b, 4 * PAGE) != 0) {
        return 2;
    }
    unsigned char *d = (unsigned char *)mmap(0, 4 * PAGE, PROT_READ | PROT_WRITE,
                                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (d != b) {
        printf("mmaptest: an interior hole was not reused (%p, wanted %p)\n",
                (void *)d, (void *)b);
        return 6;
    }

    if (mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0) != MAP_FAILED) {
        return 8;
    }
    void *guard = mmap(0, PAGE, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (guard == MAP_FAILED) {
        return 9;
    }
    munmap(guard, PAGE);
    if (mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, -1, 0) != MAP_FAILED) {
        return 8;
    }
    if (munmap((void *)&main, PAGE) == 0) {
        return 7;
    }

    extern void *malloc(unsigned long);
    extern void free(void *);
    void *first = 0;
    for (int i = 0; i < 20; i++) {
        void *p = malloc(1024 * 1024);
        if (!p) {
            return 2;
        }
        memset(p, i, 1024);
        if (i == 0) {
            first = p;
        } else if (p != first) {
            printf("mmaptest: a repeated 1 MiB malloc/free moved (%p vs %p)\n", p, first);
            return 5;
        }
        free(p);
    }

    unsigned char *many[300];
    for (int i = 0; i < 300; i++) {
        many[i] = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (many[i] == MAP_FAILED) {
            printf("mmaptest: mapping %d of 300 was refused - the region "
                   "table filled up\n", i);
            return 10;
        }
        many[i][0] = (unsigned char)i;
    }
    for (int i = 0; i < 300; i++) {
        if (many[i][0] != (unsigned char)i) {
            return 11;
        }
    }
    if (munmap(many[150], PAGE) != 0) {
        return 12;
    }
    unsigned char *refill = mmap(many[150], PAGE, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (refill != many[150]) {
        printf("mmaptest: the hole in a merged region was not reused "
               "(%p vs %p)\n", (void *)refill, (void *)many[150]);
        return 13;
    }
    refill[0] = 42;
    for (int i = 0; i < 300; i++) {
        if (i != 150 && many[i][0] != (unsigned char)i) {
            return 11;
        }
    }
    if (mprotect(many[100], PAGE, PROT_READ) != 0) {
        printf("mmaptest: mprotect inside a merged region was refused\n");
        return 14;
    }
    many[99][0] = 99;
    many[101][0] = 101;
    if (many[99][0] != 99 || many[101][0] != 101) {
        return 11;
    }
    if (mprotect(many[100], PAGE, PROT_READ | PROT_WRITE) != 0) {
        return 14;
    }
    many[100][0] = 100;

    for (int i = 0; i < 300; i++) {
        munmap(many[i], PAGE);
    }

    munmap(big + 32 * PAGE, 32 * PAGE);
    munmap(reused, 32 * PAGE);
    munmap(a, 4 * PAGE);
    munmap(c, 4 * PAGE);
    munmap(d, 4 * PAGE);
    printf("mmaptest: all checks passed\n");
    return 0;
}
