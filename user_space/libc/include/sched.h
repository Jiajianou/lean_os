/* user_space/libc/include/sched.h - M89
 *
 * What a program can ask of this scheduler, which is one thing: give up
 * the CPU.
 *
 * `sched_yield` is real - it is SYS_yield, which has existed since M8.
 * The priority calls are declared because programs reference them and
 * are truthful failures, for M65's reason: this scheduler's classes are
 * scheduler-owned and no syscall can ask for one, on the stated grounds
 * that "a flag a program sets is a flag every program sets" (M69). A
 * sched_setscheduler that returned 0 would be telling a program it had
 * been given a priority it does not have.
 */
#pragma once

#include <sys/types.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

struct sched_param {
    int sched_priority;
};

int sched_yield(void);

/* -1 with ENOSYS. See the header note - this is a refusal, not a stub. */
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);

#ifdef __cplusplus
}
#endif
