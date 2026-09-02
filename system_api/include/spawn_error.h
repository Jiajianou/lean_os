/* system_api/include/spawn_error.h
 *
 * M48: why a SYS_spawn failed.
 *
 * SYS_spawn has returned the same -1 for a missing file, a malformed ELF,
 * a full task table and out-of-memory since M13 - M40 audited all four of
 * those into existence and they remained indistinguishable to the caller,
 * which is exactly why a desktop icon that couldn't launch its program
 * could only ever "do nothing". A toast that says "no such program" is a
 * different thing from one that says "failed to launch", and the
 * difference has to come from the syscall.
 *
 * One shared message table rather than a string per call site: the
 * compositor, the desktop and the shell all want the same sentence for
 * the same code, and three copies of it is three chances for one of them
 * to say something else.
 *
 * Every code stays negative and SPAWN_ERR_NOT_FOUND stays -1, so every
 * existing `pid < 0` test - and the shell's own "command not found",
 * which is the overwhelmingly common case - keeps meaning what it meant.
 */
#pragma once

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

#define SPAWN_ERR_NOT_FOUND    (-1) /* no file by that name on disk */
#define SPAWN_ERR_BAD_IMAGE    (-2) /* the file exists but isn't a loadable x86-64 ET_EXEC (kernel/proc/elf.h's elf_validate) */
#define SPAWN_ERR_NO_TASK_SLOT (-3) /* MAX_TASKS reached - the machine is full, not the program broken */
#define SPAWN_ERR_NO_MEMORY    (-4) /* out of physical memory or heap partway through building the process */

/* A short sentence for a person, for any negative code - including one
 * this table doesn't know, which is deliberately not a special case: a
 * future error added to the kernel and not to this header should still
 * produce a toast rather than an empty one. A static inline because it is
 * three comparisons and a string, wanted by both a user program and (for
 * the boot self-test) the kernel, and giving it a translation unit of its
 * own on each side would be more machinery than the thing itself. */
static inline const char *spawn_error_message(long code) {
    switch (code) {
    case SPAWN_ERR_NOT_FOUND:
        return "No such program.";
    case SPAWN_ERR_BAD_IMAGE:
        return "That file is not a program.";
    case SPAWN_ERR_NO_TASK_SLOT:
        return "Too many programs are running.";
    case SPAWN_ERR_NO_MEMORY:
        return "Out of memory.";
    default:
        return "Could not start that program.";
    }
}

#ifdef __cplusplus
}
#endif
