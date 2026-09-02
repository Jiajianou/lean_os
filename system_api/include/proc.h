/* system_api/include/proc.h
 *
 * M45: what SYS_taskinfo reports - the process-enumeration ABI, shared
 * by kernel/arch/x86_64/syscall.c (which fills it under the scheduler's
 * own lock) and whatever wants to look at it (user_space/bin/
 * task_manager.c today). Named "proc.h" to match the kernel-side
 * kernel/proc/proc.h it is the user-visible counterpart of - the two
 * never appear on the same include path (the kernel builds with
 * -Ikernel *and* -Isystem_api/include, so a quoted "proc.h" from
 * kernel/proc/proc.c resolves same-directory-first to the kernel one,
 * exactly the resolution order wm.h's own header comment relies on;
 * kernel code that wants this file includes it as the only proc.h on the
 * -Isystem_api/include path from a directory that has no proc.h of its
 * own).
 *
 * A whole-shot snapshot, the same shape SYS_listfiles already has for
 * files: the caller sizes an array, the kernel fills as much of it as
 * fits and returns how many entries it wrote. No iterator, no handle, no
 * per-process cursor - so there is nothing here to leak, and nothing
 * that can be left half-consumed by a task that dies mid-enumeration.
 *
 * Deliberately not filtered by owner: there are no users in this OS, and
 * a permission check with nothing behind it would be the first fake one
 * in the project.
 */
#pragma once

#include <stdint.h>

/* Mirrors kernel/sched/sched.h's TASK_NAME_MAX. Duplicated rather than
 * included for the same reason file_manager.c keeps its own MAX_NAME_LEN
 * (leanfs's LEANFS_MAX_NAME): that header is kernel-only and is not on a
 * user_space build's include path at all. sys_taskinfo copies through a
 * bounded loop against *this* constant, so the two disagreeing would
 * truncate a name, never overrun a buffer. */
#define TASK_INFO_NAME_MAX 24

/* M50: how many task_info_t records a caller must be prepared for - and
 * the *definition* of the scheduler's own MAX_TASKS, which is now
 * `#define MAX_TASKS TASK_INFO_MAX` (kernel/sched/sched.h) rather than a
 * separate number.
 *
 * It became one number because two was a bug that shipped. M48 raised
 * MAX_TASKS from 64 to 128 and task_manager.c kept its own MAX_ENTRIES at
 * 64 with a comment explaining that a private constant "at least as
 * large" was fine - which it was, right up until the kernel's grew past
 * it. The task manager then silently listed only the first 64 processes,
 * so End Task acted on a long-dead task and the live one you were
 * looking for was not on screen at all. sched.h's own comment had warned
 * about exactly this ("a second hand-picked number that merely happened
 * to be >= this"); the warning was right and the mitigation wasn't.
 *
 * See sched.h for where 128 itself comes from - it is derived from the
 * boot log's "[sched] task table at handoff" measurement, not picked. */
#define TASK_INFO_MAX 128

/* Mirrors kernel/sched/sched.h's task_state_t, as plain numbers - an
 * enum whose values are part of a syscall's return payload has to be
 * pinned down here rather than inherited from a kernel header the other
 * side cannot see. */
#define TASK_INFO_READY      0
#define TASK_INFO_RUNNING    1
#define TASK_INFO_TERMINATED 2
/* M68: waiting for something - a pipe with no data, a keystroke, a child
 * to exit, a descriptor to become readable. Before M68 there was no such
 * state and every one of those tasks reported as READY, which is how a
 * desktop that never slept managed to look exactly like one that did. */
#define TASK_INFO_BLOCKED    3

typedef struct {
    int32_t pid;
    int32_t parent_pid; /* -1 for task 0 - see task_t.parent_id */
    int32_t pgid;
    int32_t state;      /* TASK_INFO_* above */
    int32_t exit_code;  /* meaningful only once state == TASK_INFO_TERMINATED */
    /* M45: the two numbers that make a resource leak visible from user
     * space for the first time. Both caps (MAX_FDS, MAX_SHM_SEGMENTS)
     * are small, fixed and never grown at runtime, and until now the
     * only way to find out how close a running desktop sat to either was
     * to hit it - which is exactly how M40's fd exhaustion stayed
     * invisible for four milestones. M50 re-derives those caps from
     * these numbers. */
    int32_t open_fds;   /* fd-table slots in use, stdin/stdout included */
    int32_t shm_segments; /* live shm segments this task created and still owns */
    char name[TASK_INFO_NAME_MAX];
} task_info_t;

/* ---- M88: SYS_rusage ---------------------------------------------------
 *
 * `who`. Two values, because times() reports both halves and getrusage()
 * names them RUSAGE_SELF and RUSAGE_CHILDREN. There is no RUSAGE_THREAD:
 * a thread here is a task with its own counters, so SELF asked from one
 * already answers about that thread plus every thread of this process
 * that has already been joined - see SYS_rusage's note on why a joined
 * thread's time lands in the process's own totals and not its
 * children's. A third constant would name a subset of that, and the
 * subset is what SELF already gives a thread that has joined nothing. */
#define OS_RUSAGE_SELF     0
#define OS_RUSAGE_CHILDREN 1

/* Two numbers, in PIT ticks at _SC_CLK_TCK (100 Hz).
 *
 * Deliberately not a `struct rusage`-shaped record with fourteen fields
 * this machine does not measure. A page-fault count, a maximum resident
 * set and a voluntary-context-switch count are all things nothing here
 * counts, and a struct full of zeros is a struct a program will divide
 * by. libc's getrusage() zeroes the fields it cannot fill - which POSIX
 * explicitly allows - but it does so at the boundary where "not
 * measured" is documented, rather than in the ABI where it would look
 * like data. */
typedef struct {
    uint64_t user_ticks;
    uint64_t sys_ticks;
} os_rusage_t;
