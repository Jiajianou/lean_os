#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL

#define VM_FILE "/tmp/vmtest.map"

static const unsigned char RET42[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};

typedef int (*fn0)(void);

__attribute__((noinline))
static unsigned long deep(unsigned long want, unsigned long *lowest) {
    volatile unsigned char pad[4096];
    pad[0] = 1;
    pad[sizeof(pad) - 1] = 2;
    unsigned long here = (unsigned long)&pad[0];
    if (*lowest == 0 || here < *lowest) {
        *lowest = here;
    }
    if (want <= sizeof(pad)) {
        return here;
    }
    unsigned long deeper = deep(want - sizeof(pad), lowest);
    return deeper + pad[0] - 1;
}

static int ordinary(void) {
    unsigned long span = 64UL * PAGE;
    void *probe = mmap(0, span, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (probe == MAP_FAILED) {
        return 2;
    }
    unsigned long arena = (unsigned long)probe;
    munmap(probe, span);

    void *hinted = mmap((void *)(arena + 8 * PAGE), PAGE, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (hinted == MAP_FAILED || (unsigned long)hinted != arena + 8 * PAGE) {
        return 2;
    }
    munmap(hinted, PAGE);

    void *four = mmap((void *)arena, 4 * PAGE, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (four == MAP_FAILED || (unsigned long)four != arena) {
        return 3;
    }
    volatile unsigned char *b = (volatile unsigned char *)four;
    for (unsigned long i = 0; i < 4; i++) {
        b[i * PAGE] = (unsigned char)(0xA0 + i);
    }
    void *mid = mmap((void *)(arena + PAGE), 2 * PAGE, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (mid == MAP_FAILED || (unsigned long)mid != arena + PAGE) {
        return 4;
    }
    if (b[0] != 0xA0 || b[3 * PAGE] != 0xA3) {
        return 4;
    }
    if (b[PAGE] != 0 || b[2 * PAGE] != 0) {
        return 4;
    }

    b[0] = 0x5A;
    if (mprotect(four, PAGE, PROT_READ) != 0) {
        return 5;
    }
    if (b[0] != 0x5A) {
        return 5;
    }
    if (mprotect(four, PAGE, PROT_READ | PROT_WRITE) != 0) {
        return 5;
    }
    b[0] = 0x5B;
    if (b[0] != 0x5B) {
        return 5;
    }

    if (mprotect((void *)(arena + 4 * PAGE), PAGE, PROT_READ) == 0) {
        return 10;
    }
    munmap(four, 4 * PAGE);

    void *code = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (code == MAP_FAILED) {
        return 6;
    }
    memcpy(code, RET42, sizeof(RET42));
    if (mprotect(code, PAGE, PROT_READ | PROT_EXEC) != 0) {
        return 6;
    }
    fn0 f = (fn0)(unsigned long)code;
    if (f() != 42) {
        return 6;
    }
    munmap(code, PAGE);

    void *scratch = mmap(0, 8 * PAGE, PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (scratch == MAP_FAILED) {
        return 7;
    }
    volatile unsigned char *s = (volatile unsigned char *)scratch;
    for (unsigned long i = 0; i < 8; i++) {
        s[i * PAGE] = 0x77;
    }
    if (madvise(scratch, 8 * PAGE, MADV_DONTNEED) != 0) {
        return 7;
    }
    for (unsigned long i = 0; i < 8; i++) {
        if (s[i * PAGE] != 0) {
            return 7;
        }
    }
    s[0] = 0x78;
    if (s[0] != 0x78) {
        return 7;
    }
    munmap(scratch, 8 * PAGE);

    unsigned long huge = 4UL * 1024 * 1024 * 1024;
    void *big = mmap(0, huge, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (big == MAP_FAILED) {
        return 8;
    }
    volatile unsigned char *h = (volatile unsigned char *)big;
    h[0] = 0x11;
    h[huge - 1] = 0x22;
    if (h[0] != 0x11 || h[huge - 1] != 0x22) {
        return 8;
    }
    munmap(big, huge);

    void *guard = mmap(0, PAGE, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (guard == MAP_FAILED) {
        return 11;
    }
    munmap(guard, PAGE);

    unsigned long lowest = 0;
    unsigned long top = (unsigned long)&lowest;
    deep(1024UL * 1024UL, &lowest);
    if (lowest == 0 || top - lowest < 900UL * 1024UL) {
        return 9;
    }

    static const char BODY[] = "m91-file-backed-mapping";
    int fd = open(VM_FILE, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return 12;
    }
    if (write(fd, BODY, sizeof(BODY)) != (long)sizeof(BODY)) {
        return 13;
    }
    char *priv = (char *)mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (priv == (char *)MAP_FAILED) {
        return 14;
    }
    if (memcmp(priv, BODY, sizeof(BODY)) != 0) {
        return 15;
    }
    for (unsigned long i = sizeof(BODY); i < PAGE; i++) {
        if (priv[i] != 0) {
            return 16;
        }
    }
    priv[0] = 'X';
    munmap(priv, PAGE);

    char back[8];
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, back, 4) != 4) {
        return 17;
    }
    if (back[0] != 'm') {
        return 18;
    }

    char *sh1 = (char *)mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char *sh2 = (char *)mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sh1 == (char *)MAP_FAILED || sh2 == (char *)MAP_FAILED || sh1 == sh2) {
        return 19;
    }
    if (memcmp(sh1, BODY, sizeof(BODY)) != 0) {
        return 20;
    }
    sh1[0] = 'Z';
    if (sh2[0] != 'Z') {
        return 21;
    }
    if (msync(sh1, PAGE, MS_SYNC) != 0) {
        return 22;
    }
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, back, 4) != 4) {
        return 23;
    }
    if (back[0] != 'Z') {
        return 24;
    }
    munmap(sh1, PAGE);
    munmap(sh2, PAGE);
    close(fd);
    unlink(VM_FILE);

    printf("vmtest: fixed, hinted, protected, executed, dropped, reserved, "
           "grown, and a file mapped both ways - all checks passed\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1] && strcmp(argv[1], "nx") == 0) {
        void *p = mmap(0, PAGE, PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) {
            return 2;
        }
        memcpy(p, RET42, sizeof(RET42));
        fn0 f = (fn0)(unsigned long)p;
        printf("vmtest: executing a page that is not PROT_EXEC returned %d\n", f());
        return 1;
    }

    if (argc > 1 && argv[1] && strcmp(argv[1], "wx") == 0) {
        volatile unsigned char *p = (volatile unsigned char *)mmap(
            0, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == (volatile unsigned char *)MAP_FAILED) {
            return 2;
        }
        p[0] = 1;
        if (mprotect((void *)p, PAGE, PROT_READ) != 0) {
            return 3;
        }
        p[0] = 2;
        printf("vmtest: writing to an mprotect'ed read-only page was allowed\n");
        return 1;
    }

    if (argc > 1 && argv[1] && strcmp(argv[1], "guard") == 0) {
        volatile unsigned char *p = (volatile unsigned char *)mmap(
            0, PAGE, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == (volatile unsigned char *)MAP_FAILED) {
            return 2;
        }
        p[0] = 1;
        printf("vmtest: touching a PROT_NONE mapping was allowed\n");
        return 1;
    }

    if (argc > 1 && argv[1] && strcmp(argv[1], "stackfar") == 0) {
        volatile unsigned char here = 0;
        volatile unsigned char *far =
            (volatile unsigned char *)((unsigned long)&here - 32UL * 1024 * 1024);
        far[0] = 1;
        printf("vmtest: touching 32 MiB below the stack pointer was allowed\n");
        return 1;
    }

    return ordinary();
}
