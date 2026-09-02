/* user_space/bin/proftest.c - M101
 *
 * The half of the profiler that has to be graded from ring 3.
 *
 * kernel.c's selftest_profile proves the instrument works: samples land,
 * they are real addresses, stopping stops it, the syscall counters move.
 * It cannot prove three things, and each of them for a specific reason:
 *
 *  - **Argument validation.** A kernel thread passing kernel pointers
 *    takes an early return out of user_range_ok - by design, and with a
 *    comment above it saying that a garbage-argument matrix run from
 *    kernel_main "would prove nothing". So the pointer refusals are
 *    checked from here, from a program with a private address space,
 *    exactly as M52 answered the same problem with badptr.c.
 *
 *  - **User-mode sampling.** Every RIP the kernel self-test can produce
 *    is a kernel RIP, because it runs in the kernel. A profiler that
 *    recorded ring-3 samples against the wrong pid, or classified them
 *    as kernel samples, would pass every assertion over there.
 *
 *  - **The capability gate actually gating.** This program holds
 *    CAP_PROCESS_LIST (see caps.h). syscalltest does not, and its whole
 *    sweep of SYS_profile is therefore refused at the gate - which makes
 *    its pass vacuous and is said so in its own table. The denial is
 *    proven by /bin/captest's sibling case; what is proven here is the
 *    other direction: that holding the bit is enough.
 *
 * Exit code 0 for pass, 1 for fail, in the shape every other self-test
 * program here uses - kernel.c spawns it and grades the code.
 */
#include "syscall_wrappers.h"

#include <profile.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("proftest: FAILED - %s\n", what);
        failures++;
    }
}

/* Somewhere a ring-3 program definitely may not write. The kernel's text
 * is identity-mapped at 1 MiB and lives below USER_REGION_BASE, so this
 * is refused by the range check rather than by a page fault - which is
 * the branch under test. */
#define A_KERNEL_ADDRESS 0x100000UL

/* Inside the canonical hole. Not mapped, not mappable, and not something
 * a range check can reason about arithmetically - it has to be rejected
 * by the region bounds. */
#define A_NONCANONICAL_ADDRESS 0x0000800000000000UL

static prof_sample_t samples[64];

/* Burns ring-3 CPU for about `ms` milliseconds. Deliberately a plain
 * arithmetic loop with no syscall in it: the point is to be sampled in
 * *user* mode, and a loop that called uptime_ms every iteration would
 * spend much of its time in the kernel and prove the opposite. */
static void burn_user(long ms) {
    long deadline = sys_uptime_ms() + ms;
    volatile unsigned long sink = 0;
    while (sys_uptime_ms() < deadline) {
        for (volatile int i = 0; i < 200000; i++) {
            sink += (unsigned long)i;
        }
    }
    (void)sink;
}

int main(void) {
    prof_stats_t st;

    /* ---- 1. The gate lets this program through ---------------------- */
    check(sys_profile(PROFILE_OP_STATS, &st, 0) == 0,
          "a program holding process-list was refused PROFILE_OP_STATS");

    /* ---- 2. Pointers this program must not be allowed to name ------- */
    check(sys_profile(PROFILE_OP_STATS, (void *)0, 0) == -1,
          "STATS accepted a null pointer");
    check(sys_profile(PROFILE_OP_STATS, (void *)A_KERNEL_ADDRESS, 0) == -1,
          "STATS wrote to a kernel address");
    check(sys_profile(PROFILE_OP_STATS, (void *)A_NONCANONICAL_ADDRESS, 0) == -1,
          "STATS accepted a non-canonical address");

    check(sys_profile(PROFILE_OP_SAMPLES, samples, 0) == -1,
          "SAMPLES accepted a count of zero");
    check(sys_profile(PROFILE_OP_SAMPLES, samples, 1 << 20) == -1,
          "SAMPLES accepted a count larger than the table");
    check(sys_profile(PROFILE_OP_SAMPLES, (void *)A_KERNEL_ADDRESS, 8) == -1,
          "SAMPLES filled a kernel address");
    check(sys_profile(PROFILE_OP_SAMPLES, (void *)0, 8) == -1,
          "SAMPLES accepted a null buffer");

    /* A buffer that starts inside this program's memory and runs off the
     * end of the region. The length is what makes it invalid, which is
     * the check an implementation that only validated the base address
     * would get wrong - and it would look completely correct until
     * something asked for a big count. */
    check(sys_profile(PROFILE_OP_SAMPLES, samples, 2048) == -1,
          "SAMPLES accepted a count its buffer cannot hold");

    check(sys_profile(PROFILE_OP_SYSCALLS, (void *)A_KERNEL_ADDRESS, 8) == -1,
          "SYSCALLS filled a kernel address");
    check(sys_profile(PROFILE_OP_SYSCALLS, samples, 1 << 20) == -1,
          "SYSCALLS accepted a count larger than the table");

    check(sys_profile(999, NULL, 0) == -1, "an operation that does not exist was accepted");
    check(sys_profile(-1, NULL, 0) == -1, "a negative operation was accepted");

    /* ---- 3. A ring-3 workload is sampled as a ring-3 workload ------- */
    sys_profile(PROFILE_OP_RESET, NULL, 0);
    check(sys_profile(PROFILE_OP_START, NULL, 0) == 0, "could not start the profiler");
    burn_user(1000);
    check(sys_profile(PROFILE_OP_STOP, NULL, 0) == 0, "could not stop the profiler");

    check(sys_profile(PROFILE_OP_STATS, &st, 0) == 0, "could not read stats back");
    /* Printed before anything is asserted about it. A split is the kind
     * of number where "the test failed" and "the test's expectation was
     * wrong" look identical in a log, and this is the line that tells
     * them apart. */
    printf("proftest: %llu samples - %llu user, %llu kernel, %llu idle\n",
           (unsigned long long)st.samples, (unsigned long long)st.user,
           (unsigned long long)st.kernel, (unsigned long long)st.idle);

    check(st.samples > 0, "a second of ring-3 CPU produced no samples at all");
    check(st.user > 0, "a ring-3 busy loop produced no user-mode samples");

    /* The assertion that catches a sampler classifying by the wrong
     * thing. This loop is user code, so user samples must outnumber
     * kernel ones - and this check failed the first time it was written,
     * at exactly 50/50, which is what led to the discovery that a task
     * halted in pit_sleep_ms was being counted as kernel time rather
     * than as idle. Restored once that was fixed, because a check that
     * was right about a real problem is worth keeping. */
    check(st.user > st.kernel,
          "a ring-3 busy loop was recorded as mostly kernel time");

    long n = sys_profile(PROFILE_OP_SAMPLES, samples, 64);
    check(n > 0, "the histogram has entries but handed none back");

    /* Every user-mode sample must carry this program's pid. A profiler
     * that recorded PROF_PID_KERNEL for ring-3 addresses would make every
     * program on the machine look like one, and the report would collide
     * their addresses - which is the reason the pid is part of the key at
     * all (every program here loads at the same base). */
    {
        long me = sys_getpid();
        int mine = 0;
        for (long i = 0; i < n; i++) {
            if (samples[i].pid == PROF_PID_KERNEL) {
                continue;
            }
            if (samples[i].pid == (int)me) {
                mine++;
            }
        }
        check(mine > 0, "no user-mode sample carried this program's pid");
    }

    /* ---- 4. The counters this program's own syscalls moved ---------- */
    {
        static prof_syscount_t counts[SYSCALL_COUNT];
        sys_profile(PROFILE_OP_SYSRESET, NULL, 0);
        for (int i = 0; i < 200; i++) {
            sys_getpid();
        }
        long got = sys_profile(PROFILE_OP_SYSCALLS, counts, SYSCALL_COUNT);
        check(got == SYSCALL_COUNT, "the syscall table came back the wrong length");
        check(counts[SYS_getpid].calls >= 200,
              "200 getpid calls from ring 3 did not reach the counter");
        check(counts[SYS_getpid].cycles == 0,
              "cycles were recorded with timing off");
    }

    if (failures == 0) {
        printf("proftest: passed - the gate, the pointer refusals, ring-3 "
               "sampling and the counters.\n");
        return 0;
    }
    printf("proftest: %d failures\n", failures);
    return 1;
}
