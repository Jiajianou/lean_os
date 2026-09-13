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

#define A_KERNEL_ADDRESS 0x100000UL

#define A_NONCANONICAL_ADDRESS 0x0000800000000000UL

static prof_sample_t samples[64];

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
    prof_statistics_t st;

    check(sys_profile(PROFILE_OP_STATISTICS, &st, 0) == 0,
          "a program holding process-list was refused PROFILE_OP_STATS");

    check(sys_profile(PROFILE_OP_STATISTICS, (void *)0, 0) == -1,
          "STATS accepted a null pointer");
    check(sys_profile(PROFILE_OP_STATISTICS, (void *)A_KERNEL_ADDRESS, 0) == -1,
          "STATS wrote to a kernel address");
    check(sys_profile(PROFILE_OP_STATISTICS, (void *)A_NONCANONICAL_ADDRESS, 0) == -1,
          "STATS accepted a non-canonical address");

    check(sys_profile(PROFILE_OP_SAMPLES, samples, 0) == -1,
          "SAMPLES accepted a count of zero");
    check(sys_profile(PROFILE_OP_SAMPLES, samples, 1 << 20) == -1,
          "SAMPLES accepted a count larger than the table");
    check(sys_profile(PROFILE_OP_SAMPLES, (void *)A_KERNEL_ADDRESS, 8) == -1,
          "SAMPLES filled a kernel address");
    check(sys_profile(PROFILE_OP_SAMPLES, (void *)0, 8) == -1,
          "SAMPLES accepted a null buffer");

    check(sys_profile(PROFILE_OP_SAMPLES, samples, 2048) == -1,
          "SAMPLES accepted a count its buffer cannot hold");

    check(sys_profile(PROFILE_OP_SYSCALLS, (void *)A_KERNEL_ADDRESS, 8) == -1,
          "SYSCALLS filled a kernel address");
    check(sys_profile(PROFILE_OP_SYSCALLS, samples, 1 << 20) == -1,
          "SYSCALLS accepted a count larger than the table");

    check(sys_profile(999, NULL, 0) == -1, "an operation that does not exist was accepted");
    check(sys_profile(-1, NULL, 0) == -1, "a negative operation was accepted");

    sys_profile(PROFILE_OP_RESET, NULL, 0);
    check(sys_profile(PROFILE_OP_START, NULL, 0) == 0, "could not start the profiler");
    burn_user(1000);
    check(sys_profile(PROFILE_OP_STOP, NULL, 0) == 0, "could not stop the profiler");

    check(sys_profile(PROFILE_OP_STATISTICS, &st, 0) == 0, "could not read stats back");
    printf("proftest: %llu samples - %llu user, %llu kernel, %llu idle\n",
           (unsigned long long)st.samples, (unsigned long long)st.user,
           (unsigned long long)st.kernel, (unsigned long long)st.idle);

    check(st.samples > 0, "a second of ring-3 CPU produced no samples at all");
    check(st.user > 0, "a ring-3 busy loop produced no user-mode samples");

    check(st.user > st.kernel,
          "a ring-3 busy loop was recorded as mostly kernel time");

    long n = sys_profile(PROFILE_OP_SAMPLES, samples, 64);
    check(n > 0, "the histogram has entries but handed none back");

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

    {
        static prof_syscall_counters_t counts[SYSCALL_COUNT];
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
