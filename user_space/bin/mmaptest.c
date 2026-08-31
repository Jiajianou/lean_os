/* user_space/bin/mmaptest.c - M78's fixture
 *
 * The milestone's own statement of proof: "A program maps and touches 64
 * pages, frees half, and a second mapping of the same size is asserted
 * to land in the freed range rather than growing the process further -
 * proof the address-space accounting tracks holes, not just a high-water
 * mark."
 *
 * So the address a mapping lands at is the thing being graded, not
 * whether the call succeeded. A high-water-mark allocator passes every
 * "did mmap work" test ever written and fails the first line of this
 * one.
 *
 * Written against <sys/mman.h> and <unistd.h>, with no path back to this
 * project's own syscall wrappers except to report the result, for the
 * same reason M77's treewalk is: the interface being tested is the
 * portable one.
 *
 * Exit codes, so a failure names itself:
 *   0  everything worked
 *   2  a mapping this program is entitled to could not be made
 *   3  the pages are not actually usable (a write did not read back)
 *   4  fresh anonymous memory was not zeroed
 *   5  a hole left by munmap was not reused - the high-water-mark bug
 *   6  a mapping in the middle of the arena was not reused
 *   7  munmap accepted an address outside the arena
 *   8  a refused flag combination was accepted
 *   9  a PROT_NONE guard mapping was refused (M91 - it used to be, and
 *      the note by the check says why that changed)
 */
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
    /* ---- 64 pages, touched end to end ------------------------------- */
    unsigned char *big = (unsigned char *)mmap(0, 64 * PAGE, PROT_READ | PROT_WRITE,
                                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (big == MAP_FAILED) {
        return 2;
    }
    /* Zeroed before anything writes to it. Anonymous memory that handed
     * back the previous owner's bytes would be one program reading
     * another's data, so this is checked before the write test rather
     * than assumed. */
    for (unsigned long i = 0; i < 64; i++) {
        if (big[i * PAGE] != 0 || big[i * PAGE + PAGE - 1] != 0) {
            return 4;
        }
    }
    if (!touch(big, 64, 0xA5)) {
        return 3;
    }

    /* ---- free half, and ask for that half back ----------------------
     *
     * THE assertion. The first 32 pages are released and a 32-page
     * mapping is requested; it has to land at exactly the address that
     * was just freed. An allocator that only remembers how far it has
     * got would hand back something past the surviving second half, and
     * the arena would grow forever under a map/free loop. */
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
    /* And the pages behind it are real and fresh, not the old ones
     * still holding 0xA5 - which they would be if munmap unmapped
     * nothing and mmap simply re-pointed at the same frames. */
    for (unsigned long i = 0; i < 32; i++) {
        if (reused[i * PAGE] != 0) {
            return 4;
        }
    }
    if (!touch(reused, 32, 0x5A)) {
        return 3;
    }
    /* The half that was NOT freed still holds what was written to it -
     * an allocator that reused the wrong range would have scribbled
     * over it. */
    for (unsigned long i = 32; i < 64; i++) {
        if (big[i * PAGE] != 0xA5) {
            return 3;
        }
    }

    /* ---- a hole in the MIDDLE, which is the harder case --------------
     *
     * Reusing the freed range above could be done by an allocator that
     * simply remembered the lowest freed address. Four mappings with the
     * second one released is the case that needs a real ordered walk. */
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

    /* ---- the refusals ------------------------------------------------
     *
     * Each of these is a way for this mmap to be quietly less than it
     * says it is, so each is checked rather than assumed.
     */
    if (mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0) != MAP_FAILED) {
        return 8; /* a shared mapping handed back as private memory */
    }
    /* M91: this used to be a refusal - "a guard page that is not one" -
     * because M78's mmap could not express "mapped but inaccessible" and
     * declined to pretend. It can now: the region exists so nothing else
     * lands there, no page is ever built for it, and touching it is
     * fatal. So the assertion is inverted rather than deleted, which is
     * the rule this project keeps arriving at - a test that encoded a
     * deliberate refusal should encode the new contract when the refusal
     * goes, not disappear. That it *dies* when touched is vmtest's
     * "guard" mode; what is checked here is that reserving one works. */
    void *guard = mmap(0, PAGE, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (guard == MAP_FAILED) {
        return 9;
    }
    munmap(guard, PAGE);
    if (mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 3, 0) != MAP_FAILED) {
        return 8; /* a file-backed mapping handed back as anonymous */
    }
    /* An address outside the arena - this program's own code. If munmap
     * accepted it, a program could unmap the instruction it is about to
     * execute. */
    if (munmap((void *)&main, PAGE) == 0) {
        return 7;
    }

    /* ---- and malloc, which is the reason any of this exists ----------
     *
     * A large allocation now goes through mmap (user_space/lib/malloc.c),
     * so a loop that allocates and frees one repeatedly must not grow
     * this process without bound. Twenty rounds of 1 MiB is 20 MiB, well
     * past what the sbrk heap could give back - which is to say: before
     * this milestone this loop was the bug, not the test. */
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

    munmap(big + 32 * PAGE, 32 * PAGE);
    munmap(reused, 32 * PAGE);
    munmap(a, 4 * PAGE);
    munmap(c, 4 * PAGE);
    munmap(d, 4 * PAGE);
    printf("mmaptest: all checks passed\n");
    return 0;
}
