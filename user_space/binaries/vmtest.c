#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL

#define VM_FILE "/tmp/vmtest.map"

static const unsigned char RET42[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};

typedef int (*fn0)(void);

/* On a page of its own, so that making that page read-only makes nothing
   else read-only - the rest of this program's data has to keep working while
   the test runs. */
__attribute__((aligned(4096)))
static volatile unsigned char protectable[4096] = {0x5A};

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

    /* M155. mprotect on a page of this program's OWN IMAGE, which mmap never
       made. POSIX applies mprotect to any mapped page and this kernel used to
       apply it only inside the mmap region, so a program that wanted part of
       its data segment read-only got a refusal. V8 is what found it: its
       read-only heap lives there, and it treats a failure to protect as
       fatal.

       protectable[] is writable static data, so it is in the image, and the
       page it sits on is the one asked about. The check is in three parts -
       the call succeeds, reading still works, and writing afterwards faults -
       and the third is the reason "image" is one of the fatal arguments
       below rather than something this run could try. */
    unsigned long page_of_data =
        (unsigned long)protectable & ~(unsigned long)(PAGE - 1);
    if (mprotect((void *)page_of_data, PAGE, PROT_READ) != 0) {
        return 15;
    }
    if (protectable[0] != 0x5A) {
        return 15;
    }
    if (mprotect((void *)page_of_data, PAGE, PROT_READ | PROT_WRITE) != 0) {
        return 15;
    }
    protectable[0] = 0x5B;
    if (protectable[0] != 0x5B) {
        return 15;
    }

    /* M156. One reservation, split until it is hundreds of regions - which is
       what PartitionAlloc does to an address space and what a fixed 128-entry
       table could not describe. Alternate pages are given different
       protections, so no two neighbours can merge and the count is the number
       of pages rather than the number of calls.

       The check is not that a particular number works. It is that the regions
       are all still THERE afterwards: every writable page holds what was
       written to it, which is only true if the table kept every split
       separate. A table that silently lost one would read back a zero. */
    enum { SPLIT_PAGES = 512 };
    unsigned char *field = (unsigned char *)mmap(0, SPLIT_PAGES * PAGE,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_ANONYMOUS | MAP_PRIVATE,
                                                 -1, 0);
    if (field == (unsigned char *)MAP_FAILED) {
        return 25;
    }
    for (unsigned long i = 0; i < SPLIT_PAGES; i += 2) {
        if (mprotect(field + i * PAGE, PAGE, PROT_READ) != 0) {
            printf("vmtest: the %lu'th split of one mapping was refused\n", i);
            return 25;
        }
    }
    for (unsigned long i = 1; i < SPLIT_PAGES; i += 2) {
        field[i * PAGE] = (unsigned char)(i & 0xFF);
    }
    for (unsigned long i = 1; i < SPLIT_PAGES; i += 2) {
        if (field[i * PAGE] != (unsigned char)(i & 0xFF)) {
            printf("vmtest: page %lu of a split mapping lost its contents\n", i);
            return 25;
        }
    }
    /* And the read-only halves are still readable, which says the splits kept
       their own protections rather than the last one winning. */
    for (unsigned long i = 0; i < SPLIT_PAGES; i += 2) {
        if (field[i * PAGE] != 0) {
            printf("vmtest: a PROT_READ page of a split mapping is not zero\n");
            return 25;
        }
    }
    if (munmap(field, SPLIT_PAGES * PAGE) != 0) {
        return 25;
    }

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

    if (argc > 1 && argv[1] && strcmp(argv[1], "image") == 0) {
        unsigned long p = (unsigned long)protectable & ~(unsigned long)(PAGE - 1);
        if (mprotect((void *)p, PAGE, PROT_READ) != 0) {
            printf("vmtest: mprotect on this program's own image was refused\n");
            return 15;
        }
        protectable[0] = 0x5C;
        printf("vmtest: writing to an image page made read-only was allowed\n");
        return 15;
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
