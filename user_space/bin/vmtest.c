/* user_space/bin/vmtest.c - M91's fixture
 *
 * M78's mmaptest asks whether a mapping works and M82's lazytest asks
 * whether it was ever built. This asks a third question neither could:
 * *where* it is, *what it is allowed to be*, and whether the kernel will
 * change its mind about that on request.
 *
 * The milestone in one sentence: a process's address space stopped being
 * six constants in a header with generous gaps between them and became a
 * set of mappings the program itself arranges - which is what a dynamic
 * loader needs (it chooses where an object goes), what a JIT needs (it
 * writes bytes and then asks for them to become code), and what a program
 * with a guard page needs (address space reserved so that touching it
 * dies).
 *
 * Modes, because four of the checks are fatal by design and a process
 * cannot report its own death:
 *
 *   (no argument)  the ordinary checks; exits 0 or a code below
 *   "nx"           executes a page that is not PROT_EXEC - must be killed
 *   "wx"           writes a page mprotect'ed read-only - must be killed
 *   "guard"        touches a PROT_NONE mapping - must be killed
 *   "stackfar"     touches far below the stack pointer - must be killed
 *
 * Exit codes for the ordinary mode, so a failure names itself:
 *   0   everything worked
 *   2   an address hint was not honoured
 *   3   MAP_FIXED did not land where it was told
 *   4   MAP_FIXED did not replace what was already there
 *   5   an mprotect round trip lost the mapping's contents
 *   6   code written to a page and then made executable would not run
 *   7   MADV_DONTNEED did not drop the pages, or broke the mapping
 *   8   a reservation far larger than the old arena was refused
 *   9   the stack would not grow deeper than its initial mapping
 *   10  mprotect accepted a range no mapping covers
 *   11  a PROT_NONE mapping was refused outright rather than reserved
 *   12-18  a file mapped MAP_PRIVATE: could not be created, did not read
 *          back as the file's own bytes, did not read as zeros past the
 *          end of the file, or a write through it reached the file -
 *          which would mean it was never private
 *   19-24  a file mapped MAP_SHARED: refused, did not read back as the
 *          file, two mappings of one page turned out not to be the same
 *          memory, or a write reached neither the other mapping nor the
 *          file after msync
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL

/* M91 (second attempt): the file checks 9 and 10 map. Under /tmp because
 * that is where every other fixture in this tree writes. */
#define VM_FILE "/tmp/vmtest.map"

/* mov eax, 42 ; ret - the smallest thing that proves a page is being
 * executed rather than merely mapped. Written as bytes because a program
 * that copies one of its own functions is copying whatever the compiler
 * decided that function was, including any relocation it needed. */
static const unsigned char RET42[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};

typedef int (*fn0)(void);

/* Recurses until it has touched `want` bytes of stack, writing to each
 * frame so the pages are really faulted in rather than merely reserved
 * by moving the stack pointer. Returns the deepest address it reached.
 *
 * `volatile` and the returned address matter: the whole point is the
 * writes, and a compiler is entitled to delete a frame nobody reads. */
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
    /* `pad` is read AFTER the recursive call on purpose. Without that the
     * call is in tail position and the compiler is entitled to turn the
     * whole recursion into a loop that reuses one frame - at which point
     * the stack never grows and this test passes a kernel that cannot
     * grow it. The volatile keeps the writes; only using the frame
     * afterwards keeps the frame. */
    return deeper + pad[0] - 1;
}

static int ordinary(void) {
    /* ---- 1. A hint, honoured -----------------------------------------
     *
     * Reserve a block, remember where it went, give it back, and then ask
     * for a page at a known-free address inside it. A kernel that ignores
     * the hint answers somewhere else, and the return value says so. */
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

    /* ---- 2. MAP_FIXED, and MAP_FIXED replacing ------------------------
     *
     * First that it lands exactly where it was told. Then the half that
     * is easy to get wrong: a fixed mapping over the middle of a live one
     * has to *replace* those pages rather than be refused, because that
     * is how a loader lays a second segment over the tail of the first
     * one's page reservation. The check is that the middle reads as the
     * fresh zero page it now is, while the pages either side keep the
     * bytes written before. */
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
        return 4; /* the pages either side were not supposed to move */
    }
    if (b[PAGE] != 0 || b[2 * PAGE] != 0) {
        return 4; /* the replaced pages were supposed to be fresh */
    }

    /* ---- 3. mprotect, both ways ---------------------------------------
     *
     * Read-only and back again, with the contents intact - which is the
     * thing that distinguishes changing permissions from remapping. */
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

    /* A range no mapping covers must be refused. Without this the two
     * checks above would pass against an mprotect that returns 0 for
     * anything, which is the failure mode a permission call must not
     * have. `arena + 4 pages` is the address just past the block, and
     * nothing was ever mapped there. */
    if (mprotect((void *)(arena + 4 * PAGE), PAGE, PROT_READ) == 0) {
        return 10;
    }
    munmap(four, 4 * PAGE);

    /* ---- 4. Write bytes, then run them --------------------------------
     *
     * The W^X round trip in the order a JIT actually does it: map
     * writable and not executable, write the code, drop write and add
     * execute, call it. A kernel with no NX bit passes the call and fails
     * the "nx" mode below; a kernel that refuses PROT_EXEC fails here. */
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

    /* ---- 5. MADV_DONTNEED ---------------------------------------------
     *
     * The pages go, the mapping stays. Both halves are checked: the bytes
     * read back as zero (they went) and writing works afterwards without
     * a new mmap (it stayed). */
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

    /* ---- 6. A reservation the old arena could not have held ------------
     *
     * 4 GiB. The arena was 160 MiB until this milestone and the whole
     * private region a process could use was under 1 GiB, so this number
     * is chosen to be impossible before M91 rather than merely large.
     * Touched at both ends, because a reservation that succeeds and
     * cannot be used proves nothing. */
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

    /* ---- 7. PROT_NONE is a mapping ------------------------------------
     *
     * Reserved, not refused. That it *dies* when touched is the "guard"
     * mode; what is checked here is that the call succeeds at all, which
     * it did not before M91. */
    void *guard = mmap(0, PAGE, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (guard == MAP_FAILED) {
        return 11;
    }
    munmap(guard, PAGE);

    /* ---- 8. A stack deeper than the one it started with ----------------
     *
     * 1 MiB, against an initial mapping of 64 KiB (USER_STACK_PAGES).
     * Sixteen times what was there is not a rounding error, and the
     * lowest address actually written is reported so the check is about
     * distance rather than about the recursion returning. */
    unsigned long lowest = 0;
    unsigned long top = (unsigned long)&lowest;
    deep(1024UL * 1024UL, &lowest);
    if (lowest == 0 || top - lowest < 900UL * 1024UL) {
        return 9;
    }

    /* ---- 9. A file, mapped private ------------------------------------
     *
     * The case M91's second attempt exists for and the one M95's loader
     * needs: a program maps a file at an address it chose and reads the
     * file's bytes there. What is checked is that the BYTES ARE THE
     * FILE'S - a mapping that succeeded and handed back zeros would pass
     * every check about addresses and none about content, which is
     * exactly the failure the old refusal was written to avoid.
     *
     * And that a write to a private mapping does NOT reach the file,
     * which is the half of "private" that a test made only of reads
     * cannot see. */
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
    /* Past the end of a 24-byte file, inside the mapped page: POSIX says
     * zeros, and a frame handed over without being cleared would say
     * whatever the last owner wrote. */
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
        return 18; /* a private write reached the file - it is not private */
    }

    /* ---- 10. A file, mapped shared -------------------------------------
     *
     * Two mappings of the same page of the same file, which must be the
     * same memory - so a write through one is visible through the other
     * without anything being flushed. That is the entire meaning of
     * MAP_SHARED and it is not observable from a single mapping.
     *
     * Then msync, and the file read back through an ordinary read(): the
     * write has to reach the disk, not just the other mapping. */
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
        return 21; /* two mappings, two frames - not shared at all */
    }
    if (msync(sh1, PAGE, MS_SYNC) != 0) {
        return 22;
    }
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, back, 4) != 4) {
        return 23;
    }
    if (back[0] != 'Z') {
        return 24; /* the shared write did not reach the file */
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
        /* Code on a page that was never PROT_EXEC. Must not return. */
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
        /* A write to a page mprotect made read-only. Must not return.
         * Distinct from lazytest's "ro", which maps read-only from the
         * start: this one was writable and stopped being, which is the
         * case that goes through vmm_protect_range_in rather than through
         * the fault handler's region lookup. */
        volatile unsigned char *p = (volatile unsigned char *)mmap(
            0, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == (volatile unsigned char *)MAP_FAILED) {
            return 2;
        }
        p[0] = 1; /* present and writable before the change */
        if (mprotect((void *)p, PAGE, PROT_READ) != 0) {
            return 3;
        }
        p[0] = 2;
        printf("vmtest: writing to an mprotect'ed read-only page was allowed\n");
        return 1;
    }

    if (argc > 1 && argv[1] && strcmp(argv[1], "guard") == 0) {
        /* A PROT_NONE mapping. Reserved so nothing else lands there, and
         * fatal to touch - which is the entire value of one. Must not
         * return. */
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
        /* Far below the stack pointer, inside the window the stack is
         * allowed to grow into. This is the case that separates "the
         * stack grows" from "any address in a 64 MiB window is silently
         * valid", and it has to stay fatal - see STACK_GROW_SLACK.
         *
         * 32 MiB below a local, which is inside USER_STACK_MAX_BYTES and
         * nowhere near the stack pointer. Must not return. */
        volatile unsigned char here = 0;
        volatile unsigned char *far =
            (volatile unsigned char *)((unsigned long)&here - 32UL * 1024 * 1024);
        far[0] = 1;
        printf("vmtest: touching 32 MiB below the stack pointer was allowed\n");
        return 1;
    }

    return ordinary();
}
