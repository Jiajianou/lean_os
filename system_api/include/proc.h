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

/* ---- M89: SYS_meminfo --------------------------------------------------
 *
 * How much physical memory there is and how much of it is free, in
 * frames. Both numbers have been in the kernel since M18 (pmm.h's
 * pmm_total_frame_count and pmm_free_frame_count) and the only way to
 * see either from user space was to read /proc/meminfo, which reports
 * one of them.
 *
 * The reason it arrived: <unistd.h>'s sysconf said of _SC_PHYS_PAGES and
 * _SC_AVPHYS_PAGES that "the kernel knows... but there is no syscall
 * that answers it... -1 is the honest answer until something asks."
 * Something asked - `free`, and `dmesg`'s uptime banner through
 * sysinfo(). Frames rather than bytes because a frame is what the PMM
 * counts, and the page size is reported alongside so the conversion is
 * the caller's and is exact.
 *
 * Not gated: how much memory the machine has is a fact about the
 * hardware, which is the line caps.h draws for SYS_fb_info.
 */
typedef struct {
    uint64_t total_frames;
    uint64_t free_frames;
    uint64_t page_size;
} os_meminfo_t;

/* ---- M96: the thread pointer, and a wait that costs nothing -----------
 *
 * SYS_arch_prctl's subfunctions. The numbers are Linux's, like every
 * other number in system_api where there is a convention to match: a
 * program compiled against a real Unix's <asm/prctl.h> means here what
 * it meant there.
 *
 * Only the FS pair. ARCH_SET_GS/ARCH_GET_GS are deliberately absent -
 * %gs is what a kernel uses for its own per-CPU data through `swapgs`,
 * and handing user space the ability to set it would be handing it the
 * register this kernel would want if it ever did. Refused by number
 * rather than accepted and ignored.
 */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

/* ---- SYS_futex's operations -------------------------------------------
 *
 * Two, and the argument for having only two is the same one <pthread.h>
 * makes for having no barriers: a futex is a *wait* and a *wake*, and
 * everything else built on one - a mutex, a condition variable, a
 * semaphore - is user-space arithmetic around those two. FUTEX_REQUEUE
 * and FUTEX_WAKE_OP are optimisations for a thundering herd that nothing
 * on this machine has measured.
 */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* ---- M95: the auxiliary vector -----------------------------------------
 *
 * What the kernel tells the dynamic linker about the program it is being
 * asked to finish loading. It sits immediately after envp's NULL in the
 * argument region, which is where every SysV runtime since 1988 looks
 * for it - the same convention that puts envp one slot past argv's NULL,
 * and for the same reason: a runtime that had to be told a second
 * address would need a second mechanism to be told it with.
 *
 * Each entry is a pair of 64-bit words, terminated by a pair whose first
 * word is AT_NULL. The numbers are Linux's, like every other number in
 * system_api where there is a convention to match.
 *
 * Only six, and each is here because the linker cannot compute it:
 *
 *   AT_PHDR/AT_PHNUM  where the PROGRAM's headers are, which is how the
 *                     linker finds its PT_DYNAMIC without opening the
 *                     file it was loaded from - and it could not open
 *                     it, because it does not know the path.
 *   AT_PHENT          the size of one, so a mismatched toolchain is a
 *                     refusal rather than a walk over garbage.
 *   AT_BASE           where the LINKER itself was placed, which is what
 *                     it needs in order to relocate itself before it can
 *                     do anything at all.
 *   AT_ENTRY          the program's entry point, which is where the
 *                     linker jumps once it is done.
 *   AT_PAGESZ         because everything else about a mapping is derived
 *                     from it.
 */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

/* M95: where the dynamic linker is placed. Inside the image window and
 * far enough from USER_IMAGE_BASE that a program of any plausible size
 * cannot reach it - 32 GiB in, against a window of 64. Fixed rather than
 * chosen per process, and deliberately: this kernel has no address-space
 * randomisation and inventing one place to put a thing that has one
 * place would be a number to keep in step with nothing. */
#define USER_INTERP_BASE 0x0000008800000000ULL

/* The path the kernel looks for an interpreter at, when a program's
 * PT_INTERP names one. Compared rather than resolved: this kernel runs
 * ONE dynamic linker, and a program asking for a different one is asking
 * for something that is not here - which is a refusal rather than a
 * substitution. */
#define USER_INTERP_PATH "/lib/ld-lean.so"

#ifdef __cplusplus
}
#endif
