/* user_space/bin/exhausttest.c - Q9
 *
 * Runs this machine out of every fixed-size resource a program can
 * exhaust, one at a time, and requires the answer to be a refusal.
 *
 * ---- what this covers that nothing else does --------------------------
 *
 * Q9 lists ten resources. Four already had an instrument before this
 * program: physical memory is /bin/oomtest (M102), the task table and
 * kernel stacks are tests/test_sched.c (Q13), and inodes and data blocks
 * are the two `slow_` tests in tests/test_leanfs.c. What had nothing at
 * all was the set a *program* holds: descriptors, pipes, shared-memory
 * segments and sockets. Those are the ones here.
 *
 * ---- the two halves, and the second is the point ----------------------
 *
 * Exhausting a resource is easy and proves little on its own: a machine
 * that halts on the last descriptor also "refuses" it, in a sense. So
 * every section here does the same three things:
 *
 *   1. take the resource until it is refused - and the refusal must be a
 *      return value, which is what says the kernel did not panic;
 *   2. give it all back;
 *   3. **take one more, successfully.** That is the half that separates
 *      a resource table which recovers from one that is merely full
 *      forever, and it is the failure this project has actually had -
 *      see M101's procfs slot leak, where the seventeenth open of any
 *      /proc file failed permanently and nothing noticed for six
 *      milestones.
 *
 * Exit codes, so the kernel self-test can say which one failed:
 *   0  every resource refused, recovered, and worked again
 *   2  descriptors: never refused
 *   3  descriptors: did not recover
 *   4  pipes: never refused
 *   5  pipes: did not recover
 *   6  shared memory: never refused
 *   7  shared memory: did not recover
 *   8  sockets: never refused
 *   9  sockets: did not recover
 *  10  a file the test needed could not be created at all
 *  11  a resource refused the very FIRST request, which is a denial
 *      rather than an exhaustion - see the note below
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

/* Larger than any of the fixed tables this machine has (MAX_FDS is 128,
 * MAX_SOCKETS and MAX_SHM_SEGMENTS are 32). A loop that stops short of
 * the limit proves nothing, and one with no bound at all cannot report
 * "it never refused" - it just runs forever. */
#define TRY_HARD 512

/* ---- zero is not a ceiling -------------------------------------------
 *
 * A resource that refuses the FIRST request has not been exhausted by
 * this program - something else is refusing it. That is not a
 * hypothetical: the socket section here refused at zero on its first
 * run, because M65's capability model gates SYS_socket and this program
 * held no CAP_NETWORK. The test would have recorded "sockets refused"
 * and been entirely wrong about why.
 *
 * So every section asserts it got at least one before it got a refusal.
 * A test that cannot tell "full" from "not allowed" is a test that
 * passes when the resource is unreachable. */
#define AT_LEAST_ONE(n) do { if ((n) == 0) { return 11; } } while (0)

int main(void) {
    /* ---- descriptors --------------------------------------------------
     *
     * The one every other section is built on: a pipe needs two, a
     * socket needs one. Done first so that a failure here is reported as
     * a descriptor failure rather than as whatever ran out downstream. */
    {
        int fd = open("/tmp/exhaust.probe", O_RDWR | O_CREAT | O_TRUNC);
        if (fd < 0) {
            return 10;
        }
        close(fd);

        static int held[TRY_HARD];
        int n = 0;
        for (; n < TRY_HARD; n++) {
            held[n] = open("/tmp/exhaust.probe", O_RDONLY);
            if (held[n] < 0) {
                break;
            }
        }
        if (n >= TRY_HARD) {
            /* It never said no. Either the table is unbounded - it is
             * not - or open() is returning a descriptor it did not
             * allocate. */
            for (int i = 0; i < n; i++) {
                close(held[i]);
            }
            return 2;
        }
        AT_LEAST_ONE(n);
        printf("exhausttest: descriptors ran out after %d opens\n", n);
        for (int i = 0; i < n; i++) {
            close(held[i]);
        }
        int again = open("/tmp/exhaust.probe", O_RDONLY);
        if (again < 0) {
            return 3;
        }
        close(again);
        unlink("/tmp/exhaust.probe");
    }

    /* ---- pipes --------------------------------------------------------
     *
     * Two descriptors each, so this runs out of one or the other and
     * either is a legitimate refusal - what matters is that it IS one. */
    {
        static int rd[TRY_HARD], wr[TRY_HARD];
        int n = 0;
        for (; n < TRY_HARD; n++) {
            int p[2];
            if (pipe(p) != 0) {
                break;
            }
            rd[n] = p[0];
            wr[n] = p[1];
        }
        if (n >= TRY_HARD) {
            for (int i = 0; i < n; i++) {
                close(rd[i]);
                close(wr[i]);
            }
            return 4;
        }
        AT_LEAST_ONE(n);
        printf("exhausttest: pipes ran out after %d pairs\n", n);
        for (int i = 0; i < n; i++) {
            close(rd[i]);
            close(wr[i]);
        }
        int p[2];
        if (pipe(p) != 0) {
            return 5;
        }
        close(p[0]);
        close(p[1]);
    }

    /* ---- shared memory ------------------------------------------------
     *
     * MAX_SHM_SEGMENTS is 32 and a segment is kernel-owned physical
     * memory, so this is the resource where "refused" and "the machine
     * is fine" are least obviously the same thing. */
    {
        /* Created AND mapped, because SYS_shm_free takes the address the
         * mapping produced - the kernel does not track who mapped what
         * (see SYS_shm_free's own note), so the only handle to a segment
         * that can be released is the one the mapper holds. That also
         * makes this the honest test: a segment nobody mapped is a
         * segment nobody could have leaked. */
        static long ids[TRY_HARD];
        static void *addrs[TRY_HARD];
        int n = 0;
        for (; n < TRY_HARD; n++) {
            ids[n] = sys_shm_create(4096);
            if (ids[n] < 0) {
                break;
            }
            long va = sys_shm_map(ids[n]);
            if (va == -1) {
                break;
            }
            addrs[n] = (void *)va;
        }
        if (n >= TRY_HARD) {
            for (int i = 0; i < n; i++) {
                sys_shm_free(ids[i], addrs[i]);
            }
            return 6;
        }
        AT_LEAST_ONE(n);
        printf("exhausttest: shm segments ran out after %d\n", n);
        for (int i = 0; i < n; i++) {
            sys_shm_free(ids[i], addrs[i]);
        }
        long again = sys_shm_create(4096);
        if (again < 0) {
            return 7;
        }
        long va = sys_shm_map(again);
        if (va != -1) {
            sys_shm_free(again, (void *)va);
        }
    }

    /* ---- sockets ------------------------------------------------------
     *
     * MAX_SOCKETS is 32, and each one carries a receive queue - so this
     * is the section that exhausts kernel memory as a side effect of
     * exhausting a table. */
    {
        static int socks[TRY_HARD];
        int n = 0;
        for (; n < TRY_HARD; n++) {
            socks[n] = (int)sys_socket(0); /* OS_SOCK_DGRAM */
            if (socks[n] < 0) {
                break;
            }
        }
        if (n >= TRY_HARD) {
            for (int i = 0; i < n; i++) {
                close(socks[i]);
            }
            return 8;
        }
        AT_LEAST_ONE(n);
        printf("exhausttest: sockets ran out after %d\n", n);
        for (int i = 0; i < n; i++) {
            close(socks[i]);
        }
        int again = (int)sys_socket(0);
        if (again < 0) {
            return 9;
        }
        close(again);
    }

    printf("exhausttest: every resource refused, recovered and worked again\n");
    return 0;
}
