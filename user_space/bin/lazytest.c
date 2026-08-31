/* user_space/bin/lazytest.c - M82's fixture
 *
 * M78's mmaptest asks whether a mapping works. This asks whether it was
 * ever built - which is a different question, and the only one M82 is
 * about.
 *
 * The whole milestone in one sentence: before this, SYS_mmap's own ABI
 * comment said "backed by real frames at the moment of the call, because
 * nothing here fills a page in on a fault", so a reservation larger than
 * the machine's memory was not a slow way to fail, it was a way to take
 * the kernel's frame allocator to zero. After it, address space is
 * reserved and a page is built when it is first touched.
 *
 * So this program reserves far more than the machine has, over and over,
 * and touches almost none of it. On an eager kernel not one line of this
 * runs. Written against <sys/mman.h> and <unistd.h> for the same reason
 * mmaptest is: the interface being tested is the portable one.
 *
 * Modes, because two of the checks are fatal by design and a process
 * cannot report its own death:
 *
 *   (no argument)  the ordinary checks; exits 0 or a code below
 *   "ro"           writes to a PROT_READ mapping - must be killed
 *   "gap"          touches arena address space nobody reserved - must be killed
 *
 * Exit codes for the ordinary mode, so a failure names itself:
 *   0  everything worked
 *   2  a reservation larger than RAM was refused - the call is still eager
 *   3  a touched page did not read back what was written to it
 *   4  an untouched page was not zero
 *   5  the cumulative reservation could not be sustained
 *   6  a read-only mapping could not be read
 *   7  an untouched mapping was refused as a syscall buffer
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096UL

/* Comfortably more than this machine has (QEMU's default is 128 MiB) and
 * inside the 160 MiB arena M82 grew - see USER_MMAP_LIMIT. The point of
 * the number is that it is bigger than physical memory, so it is written
 * as a fraction of the arena rather than as a round figure that could
 * quietly stop being bigger. */
#define HUGE_PAGES (36864UL) /* 144 MiB */

/* How many pages of the huge reservation this actually touches.
 *
 * Small against HUGE_PAGES, which is the point - but deliberately not
 * *tiny*, because the kernel-side self-test checks the frame count from
 * both sides. An upper bound alone would be passed by a test that
 * sampled before this program had mapped anything at all; the lower
 * bound is what proves the sample was taken while the reservation was
 * actually held. 256 pages is 1 MiB touched out of 144 MiB reserved. */
#define TOUCH_PAGES 256UL

/* Long enough for the boot self-test to sample the frame count while
 * this process is alive and holding its mapping. Yields rather than
 * spins so the rest of the machine keeps running.
 *
 * The exact count is not load-bearing - the self-test's own lower bound
 * on frames is what proves the sample landed - so it is set to the
 * smallest number that reliably overlaps the sampler rather than to a
 * comfortable one. It was 4000, and 4000 plus M83's two parks put 144
 * seconds on the boot: a self-test that costs more than the milestone it
 * checks is a self-test nobody will keep. */
#define PARK_YIELDS 600

extern long sys_yield(void);

static int ordinary(void) {
    /* ---- a reservation larger than the machine ---------------------- */
    unsigned char *huge = (unsigned char *)mmap(0, HUGE_PAGES * PAGE,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (huge == MAP_FAILED) {
        return 2;
    }

    /* Touch a handful of pages, spread across the whole reservation so
     * that the pages built are demonstrably not just the first few. */
    unsigned long stride = HUGE_PAGES / TOUCH_PAGES;
    for (unsigned long i = 0; i < TOUCH_PAGES; i++) {
        huge[i * stride * PAGE] = (unsigned char)(i + 1);
    }
    for (unsigned long i = 0; i < TOUCH_PAGES; i++) {
        if (huge[i * stride * PAGE] != (unsigned char)(i + 1)) {
            return 3;
        }
    }

    /* A page nobody touched reads as zero. This is the check that a
     * demand-filled page is zeroed rather than handed over with whatever
     * the previous owner left in it - and it has to be a page far from
     * the ones written above, so it is genuinely being built by this
     * read rather than having been built already. */
    if (huge[(HUGE_PAGES / 2) * PAGE + 7] != 0) {
        return 4;
    }

    /* Park, so the kernel-side self-test can sample the frame count while
     * 144 MiB is reserved and sixteen pages are built. */
    printf("lazytest: %lu MiB reserved, %lu pages touched - parking\n",
           (HUGE_PAGES * PAGE) / (1024UL * 1024UL), TOUCH_PAGES);
    for (int i = 0; i < PARK_YIELDS; i++) {
        sys_yield();
    }

    if (munmap(huge, HUGE_PAGES * PAGE) != 0) {
        return 5;
    }

    /* ---- more than the machine has, cumulatively --------------------
     *
     * Eight rounds of 144 MiB is over a gigabyte reserved on a 128 MiB
     * machine. Each round is released before the next, so this is not a
     * claim about the arena's size - it is a claim that reserving costs
     * no memory, because a kernel that backed even a tenth of one round
     * would be out of frames before the second. */
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

    /* ---- an untouched mapping as a syscall buffer -------------------
     *
     * The most ordinary thing anyone does with an mmap, and the one that
     * demand paging is most likely to have broken invisibly: hand the
     * kernel a buffer that has been reserved and never written to. The
     * kernel checks a caller's buffer by walking its page tables, and an
     * untouched page has no entry to find - so without the prefault in
     * user_range_ok this read returns -1 and every program that ever
     * mmaps a buffer to read into is broken.
     *
     * getcwd is used because it writes into the buffer, is always
     * available, and has an answer this can check without needing a file
     * to exist. */
    char *buf = (char *)mmap(0, 16 * PAGE, PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (buf == MAP_FAILED) {
        return 2;
    }
    if (getcwd(buf, 16 * PAGE) == 0 || buf[0] != '/') {
        printf("lazytest: an untouched mmap buffer was refused as a syscall argument\n");
        return 7;
    }
    munmap(buf, 16 * PAGE);

    /* ---- a read-only mapping is readable ---------------------------- */
    unsigned char *ro = (unsigned char *)mmap(0, 4 * PAGE, PROT_READ,
                                               MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (ro == MAP_FAILED) {
        return 2;
    }
    /* A read fault on a PROT_READ page must be filled, not fatal - the
     * mapping is legitimate and the access is the one it promised. */
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
        /* A write to a mapping asked for read-only. The page is not
         * present, so this arrives as the same not-present fault a first
         * touch does - which is exactly why sched_fault_fill has to check
         * the region's prot rather than assume any fault inside a mapping
         * is fillable. Must not return. */
        volatile unsigned char *ro = (volatile unsigned char *)mmap(
            0, PAGE, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (ro == (volatile unsigned char *)MAP_FAILED) {
            return 2;
        }
        /* volatile, because the whole point of this store is the fault it
         * takes and nothing ever reads it back - a compiler is entitled
         * to delete a dead store, and a test whose failing operation gets
         * optimised away passes for the wrong reason. */
        ro[0] = 1;
        printf("lazytest: writing to a PROT_READ mapping was allowed\n");
        return 1;
    }

    if (argc > 1 && argv[1] && strcmp(argv[1], "gap") == 0) {
        /* Address space inside the arena that this process never
         * reserved. A fault here is a wild pointer and has to stay one:
         * an arena that filled any address touched inside it would have
         * turned every out-of-bounds access into silent success. Must not
         * return.
         *
         * Found by reserving a page and walking well past its end, rather
         * than by naming an address - USER_MMAP_BASE is a kernel constant
         * and a program has no business knowing it. */
        volatile unsigned char *p = (volatile unsigned char *)mmap(
            0, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (p == (volatile unsigned char *)MAP_FAILED) {
            return 2;
        }
        p[64UL * 1024UL * 1024UL] = 1; /* volatile - see the "ro" mode above */
        printf("lazytest: touching unreserved arena address space was allowed\n");
        return 1;
    }

    return ordinary();
}
