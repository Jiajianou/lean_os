#include "syscall_wrappers.h"

#include "syscall.h" /* system_api/include/syscall.h - the shared ABI */

/* Matches system_api/include/syscall.h's convention exactly: rax =
 * number/return, rdi/rsi/rdx = args 1-3. Same helper shape as the
 * kernel's own self-test one (kernel/kernel.c) - this is the real,
 * reusable version every user program links against instead of hand-
 * rolling its own inline asm. */
static long do_syscall(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r8", "r9", "memory");
    return ret;
}

long sys_write(int fd, const void *buf, size_t len) {
    return do_syscall(SYS_write, fd, (long)buf, (long)len);
}

void sys_exit(int code) {
    do_syscall(SYS_exit, code, 0, 0);
    for (;;) {
        /* Unreachable: SYS_exit terminates this task and never returns.
         * The loop only exists so the compiler doesn't have to take that
         * on faith - `noreturn` with a plain fall-through would warn. */
    }
}

long sys_getpid(void) {
    return do_syscall(SYS_getpid, 0, 0, 0);
}

long sys_spawn(const char *path, const char *arg) {
    return do_syscall(SYS_spawn, (long)path, (long)arg, 0);
}

long sys_wait(long pid) {
    return do_syscall(SYS_wait, pid, 0, 0);
}

long sys_read(int fd, void *buf, size_t len) {
    return do_syscall(SYS_read, fd, (long)buf, (long)len);
}

long sys_readfile(const char *name, void *buf, size_t maxlen) {
    return do_syscall(SYS_readfile, (long)name, (long)buf, (long)maxlen);
}

long sys_listfiles(void *buf, size_t maxlen) {
    return do_syscall(SYS_listfiles, (long)buf, (long)maxlen, 0);
}

long sys_kill(long pid, int sig) {
    return do_syscall(SYS_kill, pid, sig, 0);
}

long sys_pipe(int fds_out[2]) {
    return do_syscall(SYS_pipe, (long)fds_out, 0, 0);
}

long sys_getpgid(long pid) {
    return do_syscall(SYS_getpgid, pid, 0, 0);
}
