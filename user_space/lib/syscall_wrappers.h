/* user_space/lib/syscall_wrappers.h
 *
 * Thin C wrappers around the syscalls defined in system_api/include/
 * syscall.h - one function per syscall, matching its arguments, hiding
 * the `int 0x80` calling convention. Named "syscall_wrappers.h" rather
 * than the more obvious "syscall.h" so a quoted #include from this
 * file's own .c doesn't resolve to itself instead of the shared ABI
 * header - the exact collision kernel/arch/x86_64/syscall_entry.h was
 * renamed to avoid.
 */
#pragma once

#include <stddef.h>

long sys_write(int fd, const void *buf, size_t len);
void sys_exit(int code) __attribute__((noreturn));
long sys_getpid(void);

/* arg may be NULL for a program that doesn't take one. Returns the new
 * process's pid, or -1 if `path` doesn't exist on disk. */
long sys_spawn(const char *path, const char *arg);
/* Blocks (cooperatively) until `pid` has terminated; returns its exit
 * code, or -1 if `pid` was never valid. */
long sys_wait(long pid);

/* Only fd=0 (stdin) works. Blocks until at least one byte is available. */
long sys_read(int fd, void *buf, size_t len);

/* Whole-file read by name. Returns the file's size (may exceed maxlen,
 * in which case only maxlen bytes were actually copied) or -1 if it
 * doesn't exist. */
long sys_readfile(const char *name, void *buf, size_t maxlen);

/* Newline-separated filenames into buf. Returns bytes written, or -1. */
long sys_listfiles(void *buf, size_t maxlen);

/* Only SIGKILL/SIGTERM (system_api/include/signal.h) are recognized.
 * Returns 0, or -1 if pid doesn't name a live task. */
long sys_kill(long pid, int sig);

/* Installs a pipe's read/write ends into the caller's own fd table and
 * writes their fd numbers to fds_out[2] (fds_out[0] = read end,
 * fds_out[1] = write end) - a child spawned afterward (sys_spawn)
 * inherits both. Returns 0, or -1 on failure (no free fd slots, or out
 * of memory). */
long sys_pipe(int fds_out[2]);

long sys_getpgid(long pid);
