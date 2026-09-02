/* system_api/include/profile.h
 *
 * M101: the kernel/user contract for the profiler and the syscall
 * accounting - the operations, and the two structs that cross the
 * boundary.
 *
 * ---- Why this is a syscall and not a /proc file ------------------------
 *
 * /proc/profile and /proc/syscalls exist too, and they are the right
 * interface for a person with a terminal. They are not the right one for
 * a tool, for two reasons this file exists to avoid:
 *
 *  - reading is only half of it. Starting, stopping and resetting a
 *    profile are *writes*, and procfs refuses every write on purpose
 *    (see kernel/fs/procfs.c: "a program that writes to
 *    /proc/self/status has misunderstood something"). Carving out an
 *    exception for one file would be the beginning of a control plane
 *    made of string parsing.
 *  - a sample is a number. Formatting 2048 of them as decimal text so
 *    that a tool can parse them back is work done twice, and the parse
 *    is the half that can be wrong.
 *
 * So: the syscall is the machine interface, /proc is the human one, and
 * both read the same tables.
 *
 * ---- The gate ---------------------------------------------------------
 *
 * CAP_PROCESS_LIST, the same capability the task manager holds. A
 * profile is a description of what every process on the machine is
 * doing, down to the instruction - which is strictly more than the task
 * list already gated behind that bit, and there is no reading of the
 * capability model in which the coarse view is privileged and the fine
 * one is not.
 */
#pragma once

#include <stdint.h>

/* One entry of the flat profile: an address, how many ticks were
 * sampled at it, and whose address it is. */
typedef struct {
    uint64_t rip;
    uint64_t count;
    int32_t pid;   /* PROF_PID_KERNEL (-1) for a ring-0 sample */
    int32_t _pad;  /* explicit, so the struct's size is the same number
                    * on both sides of the boundary rather than the same
                    * number by luck */
} prof_sample_t;

#define PROF_PID_KERNEL (-1)

/* What the run itself did, which is the context every percentage in a
 * report needs. A histogram without these numbers cannot tell you that
 * the machine was 90% idle for the whole measurement. */
typedef struct {
    uint64_t samples;
    uint64_t kernel;
    uint64_t user;
    uint64_t idle;
    uint64_t overflow; /* samples the table had no room for - see PROF_BUCKETS */
    uint64_t distinct;
    int32_t running;
    int32_t _pad;
} prof_stats_t;

/* Per-syscall accounting. `cycles` is 0 for calls made while timing was
 * off, which is the default - see PROFILE_OP_TIMING. */
typedef struct {
    uint64_t calls;
    uint64_t cycles;
} prof_syscount_t;

/* ---- SYS_profile(op, arg1, arg2) -------------------------------------- */

#define PROFILE_OP_START     0 /* () -> 0 */
#define PROFILE_OP_STOP      1 /* () -> 0 */
#define PROFILE_OP_RESET     2 /* () -> 0. Clears the histogram, not the syscall table */
#define PROFILE_OP_STATS     3 /* (prof_stats_t *out) -> 0 or -1 */
#define PROFILE_OP_SAMPLES   4 /* (prof_sample_t *out, max) -> entries written */
#define PROFILE_OP_SYSCALLS  5 /* (prof_syscount_t *out, max) -> entries written, indexed by syscall number */
#define PROFILE_OP_SYSRESET  6 /* () -> 0 */
#define PROFILE_OP_TIMING    7 /* (on) -> the previous setting */
