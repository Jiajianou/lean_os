#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/resource.h>
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

/* M204: a memfd's frames belong to the memfd, and a mapping of it holds no
   reference on them - so neither dropping the mapping's pages nor unmapping
   it may free them. madvise(MADV_DONTNEED) over a shared mapping did: the
   memfd went on naming frames the allocator had handed to somebody else.
   Fresh anonymous memory is dirtied after each, which is what takes a
   wrongly freed frame back out of the allocator, and the memfd has to come
   back with what was written into it. */
#define BORROWED_PAGES 64UL

static unsigned char borrowed_mark(unsigned long page) {
    return (unsigned char)(0xA0u + (page % 31u));
}

static void dirty_fresh_memory(void) {
    unsigned long pages = BORROWED_PAGES * 4;
    unsigned char *fresh = (unsigned char *)mmap(0, pages * PAGE, PROT_READ | PROT_WRITE,
                                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (fresh == (unsigned char *)MAP_FAILED) {
        return;
    }
    memset(fresh, 0x5A, pages * PAGE);
    munmap(fresh, pages * PAGE);
}

static int memfd_holds_marks(int fd) {
    unsigned char *view = (unsigned char *)mmap(0, BORROWED_PAGES * PAGE, PROT_READ, MAP_SHARED, fd, 0);
    if (view == (unsigned char *)MAP_FAILED) {
        return 0;
    }
    int intact = 1;
    for (unsigned long i = 0; i < BORROWED_PAGES; i++) {
        if (view[i * PAGE] != borrowed_mark(i) || view[i * PAGE + PAGE - 1] != borrowed_mark(i)) {
            printf("vmtest: memfd page %lu holds %02x, not %02x\n", i, view[i * PAGE], borrowed_mark(i));
            intact = 0;
            break;
        }
    }
    munmap(view, BORROWED_PAGES * PAGE);
    return intact;
}

static int borrowed_frames(void) {
    int fd = memfd_create("vmtest", 0);
    if (fd < 0 || ftruncate(fd, (off_t)(BORROWED_PAGES * PAGE)) != 0) {
        return 26;
    }
    unsigned char *shared = (unsigned char *)mmap(0, BORROWED_PAGES * PAGE, PROT_READ | PROT_WRITE,
                                                  MAP_SHARED, fd, 0);
    if (shared == (unsigned char *)MAP_FAILED) {
        return 26;
    }
    for (unsigned long i = 0; i < BORROWED_PAGES; i++) {
        shared[i * PAGE] = borrowed_mark(i);
        shared[i * PAGE + PAGE - 1] = borrowed_mark(i);
    }
    if (madvise(shared, BORROWED_PAGES * PAGE, MADV_DONTNEED) != 0) {
        return 26;
    }
    dirty_fresh_memory();
    if (!memfd_holds_marks(fd)) {
        printf("vmtest: madvise(MADV_DONTNEED) over a shared memfd mapping freed the memfd's frames\n");
        return 27;
    }
    munmap(shared, BORROWED_PAGES * PAGE);
    dirty_fresh_memory();
    if (!memfd_holds_marks(fd)) {
        printf("vmtest: munmap of a shared memfd mapping freed the memfd's frames\n");
        return 28;
    }
    close(fd);
    return 0;
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

    /* M169. A system call that cannot write its answer says so.

       This is the protected-memory pattern, and it belongs here rather than
       beside the other resource-limit checks because what it is about is a
       page rather than a limit: Chromium makes a page of its own data
       read-only, hands its address to getrlimit(2), and takes -1 with EFAULT
       as its proof that the protection took. A getrlimit answered inside the
       C library writes through the pointer and the caller takes SIGSEGV
       instead, which is what happened to every renderer this machine
       started. So the call has to reach the kernel, because the kernel is
       the only thing here that can look at a page table before it writes.

       The second half is the part that makes the first non-vacuous: the same
       call with the same pointer must SUCCEED once the page is writable
       again, and must answer with this machine's own numbers. A getrlimit
       that always failed would pass the check above. */
    if (mprotect((void *)page_of_data, PAGE, PROT_READ) != 0) {
        return 16;
    }
    struct rlimit probe_limit;
    errno = 0;
    if (getrlimit(RLIMIT_NPROC, (struct rlimit *)page_of_data) != -1 ||
        errno != EFAULT) {
        printf("vmtest: getrlimit wrote into a read-only page (errno %d)\n",
               errno);
        return 16;
    }
    if (mprotect((void *)page_of_data, PAGE, PROT_READ | PROT_WRITE) != 0) {
        return 16;
    }
    if (getrlimit(RLIMIT_NPROC, (struct rlimit *)page_of_data) != 0) {
        return 16;
    }
    if (getrlimit(RLIMIT_NOFILE, &probe_limit) != 0 ||
        probe_limit.rlim_cur != (rlim_t)OPEN_MAX) {
        printf("vmtest: getrlimit(RLIMIT_NOFILE) is not this machine's "
               "descriptor table\n");
        return 16;
    }
    errno = 0;
    if (getrlimit(RLIM_NLIMITS + 1, &probe_limit) != -1 || errno != EINVAL) {
        printf("vmtest: getrlimit accepted a resource that does not exist\n");
        return 16;
    }
    errno = 0;
    if (setrlimit(RLIMIT_NOFILE, (const struct rlimit *)0) != -1 ||
        errno != EFAULT) {
        printf("vmtest: setrlimit read through a null pointer\n");
        return 16;
    }
    /* A limit that cannot move says so. The descriptor table is a fixed
       array in the task, so asking for a different ceiling is EPERM rather
       than a zero that changes nothing - M65's rule, on a call that has a
       return value for exactly this. */
    probe_limit.rlim_cur = probe_limit.rlim_max = 4;
    errno = 0;
    if (setrlimit(RLIMIT_NOFILE, &probe_limit) != -1 || errno != EPERM) {
        printf("vmtest: setrlimit claimed to lower a ceiling that is fixed\n");
        return 16;
    }

    /* M169. And the range a protection is asked for is checked to its end.
       sys_mprotect handed its "is every page of this range really mapped"
       check a PAGE COUNT where it wanted BYTES, so for any range the number
       fell inside the first page and only that page was ever looked at - a
       range running off the end of the image was accepted and half applied.

       The first unmapped page above this image is found by asking, one page
       at a time, for the protection those pages already have; then a range
       ending in that page must be refused. */
    unsigned long walk = page_of_data;
    for (int steps = 0; steps < 65536; steps++) {
        if (mprotect((void *)(walk + PAGE), PAGE, PROT_READ | PROT_WRITE) != 0) {
            break;
        }
        walk += PAGE;
    }
    if (mprotect((void *)walk, 2 * PAGE, PROT_READ | PROT_WRITE) == 0) {
        printf("vmtest: mprotect accepted a range running off the image\n");
        return 17;
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

    int borrowed = borrowed_frames();
    if (borrowed != 0) {
        return borrowed;
    }

    printf("vmtest: fixed, hinted, protected, executed, dropped, reserved, "
           "grown, a file mapped both ways, a system call refusing to write "
           "a read-only page, a range running off the image refused, and a "
           "memfd's frames kept through madvise and munmap - "
           "all checks passed\n");
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
