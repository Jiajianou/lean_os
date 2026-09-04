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

/* M64: the same trap with the three argument registers the kernel's
 * dispatcher has always passed and nothing had ever used. SYS_sendto is
 * the first call here with five arguments, and packing them into a
 * struct to keep a three-argument wrapper would have been a second ABI
 * to describe rather than a smaller one. rcx is safe to pass in because
 * this is `int`/`iret`, not `syscall`/`sysret` - only the latter
 * commandeers it for the return address. */
static long do_syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3), "c"(a4), "r"(r8), "r"(r9)
                      : "memory");
    return ret;
}

long sys_raw(long num, long a1, long a2, long a3) {
    return do_syscall(num, a1, a2, a3);
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

long sys_spawnve(const char *path, const char *const *argv, const char *const *envp) {
    return do_syscall(SYS_spawn, (long)path, (long)argv, (long)envp);
}

long sys_spawnv(const char *path, const char *const *argv) {
    /* M75: `environ`, not NULL. The kernel's NULL case means "the
     * environment this process was *started* with", which is right for a
     * raw syscall and wrong for a program that has just called setenv -
     * so the wrapper every program actually uses passes the live one.
     * environ is NULL only in a program linked without libc's startup,
     * which cannot happen here (crt0 calls it), and the kernel treats a
     * NULL envp as inherit, so even that degrades to the old behaviour
     * rather than to an empty environment. */
    extern char **environ;
    return sys_spawnve(path, argv, (const char *const *)environ);
}

/* M99: a fourth argument, so do_syscall6 rather than do_syscall. The
 * kernel reads exactly one bit of it - SA_SIGINFO - and that bit is a
 * calling convention: it decides whether the handler is entered with
 * one argument or with three. */
long sys_sigaction(int signo, void *handler, void (*restorer)(void),
                   unsigned int flags) {
    return do_syscall6(SYS_sigaction, signo, (long)handler, (long)restorer,
                       (long)flags, 0, 0);
}

long sys_sigprocmask(int how, unsigned int mask, unsigned int *old_out) {
    return do_syscall(SYS_sigprocmask, how, (long)mask, (long)old_out);
}

long sys_fstat(int fd, os_stat_t *out) {
    return do_syscall(SYS_fstat, fd, (long)out, 0);
}

long sys_thread_create(void *entry, void *arg, unsigned long stack_top) {
    return do_syscall(SYS_thread_create, (long)entry, (long)arg, (long)stack_top);
}

void sys_thread_exit(int value) {
    do_syscall(SYS_thread_exit, value, 0, 0);
    for (;;) {
        /* Unreachable: SYS_thread_exit ends this thread. Same shape as
         * sys_exit's own loop, and for the same reason - `noreturn` with
         * a plain fall-through would warn. */
    }
}

long sys_gettid(void) {
    return do_syscall(SYS_gettid, 0, 0, 0);
}

long sys_mmap(void *addr, unsigned long len, int prot, int flags, int fd,
              unsigned long offset) {
    return do_syscall6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, (long)offset);
}

long sys_link(const char *old_path, const char *new_path) {
    return do_syscall(SYS_link, (long)old_path, (long)new_path, 0);
}

long sys_fsync(int fd) {
    return do_syscall(SYS_fsync, fd, 0, 0);
}

long sys_mprotect(void *addr, unsigned long len, int prot) {
    return do_syscall(SYS_mprotect, (long)addr, (long)len, prot);
}

long sys_madvise(void *addr, unsigned long len, int advice) {
    return do_syscall(SYS_madvise, (long)addr, (long)len, advice);
}

long sys_munmap(void *addr, unsigned long len) {
    return do_syscall(SYS_munmap, (long)addr, (long)len, 0);
}

long sys_chdir(const char *path) {
    return do_syscall(SYS_chdir, (long)path, 0, 0);
}

long sys_getcwd(char *buf, size_t maxlen) {
    return do_syscall(SYS_getcwd, (long)buf, (long)maxlen, 0);
}

long sys_spawn(const char *path, const char *arg) {
    /* M60: one argument is a vector of one. The kernel supplies argv[0]
     * (the path), so this array holds only what the caller had to say. */
    const char *v[2];
    int n = 0;
    if (arg && arg[0]) {
        v[n++] = arg;
    }
    v[n] = (const char *)0;
    return sys_spawnv(path, v);
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

long sys_listdir(const char *path, void *buf, size_t maxlen) {
    return do_syscall(SYS_listdir, (long)path, (long)buf, (long)maxlen);
}

/* M81: the streaming counterpart. `cookie` is opaque - zero to start, and
 * handed straight back untouched on every following call. Returns the
 * bytes of os_dirent_t records written, 0 at the end of the directory, or
 * -1. See SYS_getdents in system_api/include/syscall.h. */
long sys_getdents(const char *path, unsigned int *cookie, void *buf, size_t buflen) {
    return do_syscall6(SYS_getdents, (long)path, (long)cookie, (long)buf,
                       (long)buflen, 0, 0);
}

/* M83: the child's pid in the parent, 0 in the child, -1 on failure.
 *
 * No arguments, and none possible: the two sides of a fork differ only in
 * what this returns. See SYS_fork in system_api/include/syscall.h for
 * what the child does and does not inherit. */
long sys_fork(void) {
    return do_syscall(SYS_fork, 0, 0, 0);
}

/* M84: does not return on success. See SYS_execve. */
long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    return do_syscall(SYS_execve, (long)path, (long)argv, (long)envp);
}

long sys_waitpid(long pid, int *status, long options) {
    return do_syscall6(SYS_waitpid, pid, (long)status, options, 0, 0, 0);
}

long sys_fcntl(int fd, int cmd, long arg) {
    return do_syscall(SYS_fcntl, fd, cmd, arg);
}

/* M85: process groups and sessions. See SYS_setpgid. */
long sys_setpgid(long pid, long pgid) {
    return do_syscall(SYS_setpgid, pid, pgid, 0);
}

long sys_setsid(void) {
    return do_syscall(SYS_setsid, 0, 0, 0);
}

long sys_getsid(long pid) {
    return do_syscall(SYS_getsid, pid, 0, 0);
}

long sys_ioctl(int fd, unsigned long cmd, void *arg) {
    return do_syscall(SYS_ioctl, fd, (long)cmd, (long)arg);
}

/* M87: set a file's length. See SYS_ftruncate. */
long sys_ftruncate(int fd, long length) {
    return do_syscall(SYS_ftruncate, fd, length, 0);
}

/* M87: symbolic links. Note the argument order on symlink - target
 * first, then the name to create - which is symlink(2)'s everywhere. */
long sys_symlink(const char *target, const char *path) {
    return do_syscall(SYS_symlink, (long)target, (long)path, 0);
}

long sys_readlink(const char *path, char *buf, size_t len) {
    return do_syscall(SYS_readlink, (long)path, (long)buf, (long)len);
}

long sys_lstat(const char *path, void *out) {
    return do_syscall(SYS_lstat, (long)path, (long)out, 0);
}

/* M88: the three a build probes for. See the ABI notes in
 * system_api/include/syscall.h. */
long sys_rusage(int who, void *out) {
    return do_syscall(SYS_rusage, who, (long)out, 0);
}

long sys_statvfs(const char *path, void *out) {
    return do_syscall(SYS_statvfs, (long)path, (long)out, 0);
}

/* M89: the absolute path an open descriptor was opened with, for the
 * *at() family - see SYS_fdpath. */
long sys_fdpath(int fd, char *out, unsigned long out_len) {
    return do_syscall(SYS_fdpath, fd, (long)out, (long)out_len);
}

long sys_getppid(void) {
    return do_syscall(SYS_getppid, 0, 0, 0);
}

long sys_sync(void) {
    return do_syscall(SYS_sync, 0, 0, 0);
}

long sys_meminfo(void *out) {
    return do_syscall(SYS_meminfo, (long)out, 0, 0);
}

long sys_alarm(unsigned int seconds) {
    return do_syscall(SYS_alarm, (long)seconds, 0, 0);
}

long sys_msync(void *addr, unsigned long len, int flags) {
    return do_syscall(SYS_msync, (long)addr, (long)len, flags);
}

/* M96: the thread pointer and the futex - see SYS_arch_prctl and
 * SYS_futex for what each argument means and what the returns are. */
long sys_arch_prctl(int code, unsigned long addr) {
    return do_syscall(SYS_arch_prctl, code, (long)addr, 0);
}

long sys_futex(volatile unsigned int *addr, int op, unsigned int val,
               unsigned int timeout_ms) {
    return do_syscall6(SYS_futex, (long)addr, op, (long)val, (long)timeout_ms, 0, 0);
}

long sys_utime(const char *path, unsigned int mtime) {
    return do_syscall(SYS_utime, (long)path, (long)mtime, 0);
}

long sys_mkdir(const char *path) {
    return do_syscall(SYS_mkdir, (long)path, 0, 0);
}

long sys_unlink(const char *path) {
    return do_syscall(SYS_unlink, (long)path, 0, 0);
}

long sys_rename(const char *old_path, const char *new_path) {
    return do_syscall(SYS_rename, (long)old_path, (long)new_path, 0);
}

long sys_shm_unmap(void *vaddr, unsigned long bytes) {
    return do_syscall(SYS_shm_unmap, (long)vaddr, (long)bytes, 0);
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

long sys_sbrk(long increment) {
    return do_syscall(SYS_sbrk, increment, 0, 0);
}

long sys_shm_create(size_t size) {
    return do_syscall(SYS_shm_create, (long)size, 0, 0);
}

long sys_shm_map(long id) {
    return do_syscall(SYS_shm_map, id, 0, 0);
}

long sys_fb_info(wm_fb_info_t *out) {
    return do_syscall(SYS_fb_info, (long)out, 0, 0);
}

long sys_fb_map(void) {
    return do_syscall(SYS_fb_map, 0, 0, 0);
}

long sys_display_modes(display_mode_t *out, long max) {
    return do_syscall(SYS_display_modes, (long)out, max, 0);
}

long sys_display_set_mode(uint32_t width, uint32_t height) {
    return do_syscall(SYS_display_set_mode, (long)width, (long)height, 0);
}

long sys_open(const char *path, uint32_t flags) {
    return do_syscall(SYS_open, (long)path, (long)flags, 0);
}

long sys_lseek(int fd, long offset, int whence) {
    return do_syscall(SYS_lseek, fd, offset, whence);
}

long sys_stat(const char *path, os_stat_t *out) {
    return do_syscall(SYS_stat, (long)path, (long)out, 0);
}

long sys_rmdir(const char *path) {
    return do_syscall(SYS_rmdir, (long)path, 0, 0);
}

long sys_time(os_datetime_t *out) {
    return do_syscall(SYS_time, (long)out, 0, 0);
}

/* ---- M65: capabilities ----------------------------------------------- */

long sys_getcaps(void) {
    return do_syscall(SYS_getcaps, 0, 0, 0);
}

long sys_dropcaps(uint32_t keep) {
    return do_syscall(SYS_dropcaps, (long)keep, 0, 0);
}

/* ---- M64: the network ------------------------------------------------ */

long sys_socket(int type) {
    return do_syscall(SYS_socket, type, 0, 0);
}

long sys_listen(int fd) {
    return do_syscall(SYS_listen, fd, 0, 0);
}

long sys_connect(int fd, uint32_t ip, uint16_t port) {
    return do_syscall(SYS_connect, fd, (long)ip, port);
}

long sys_connstat(int fd) {
    return do_syscall(SYS_connstat, fd, 0, 0);
}

long sys_accept(int fd, os_sockaddr_t *from) {
    return do_syscall(SYS_accept, fd, (long)from, 0);
}

long sys_send(int fd, const void *data, uint32_t len) {
    return do_syscall(SYS_send, fd, (long)data, (long)len);
}

long sys_recv(int fd, void *data, uint32_t max) {
    return do_syscall(SYS_recv, fd, (long)data, (long)max);
}

long sys_bind(int fd, uint16_t port) {
    return do_syscall(SYS_bind, fd, port, 0);
}

long sys_sendto(int fd, uint32_t ip, uint16_t port, const void *data, uint32_t len) {
    return do_syscall6(SYS_sendto, fd, (long)ip, port, (long)data, (long)len, 0);
}

long sys_recvfrom(int fd, void *data, uint32_t max, os_sockaddr_t *from) {
    return do_syscall6(SYS_recvfrom, fd, (long)data, (long)max, (long)from, 0, 0);
}

long sys_sockpoll(int fd) {
    return do_syscall(SYS_sockpoll, fd, 0, 0);
}

long sys_netconf(os_netconf_t *out) {
    return do_syscall(SYS_netconf, (long)out, 0, 0);
}

long sys_settime(uint32_t unix_seconds) {
    return do_syscall(SYS_settime, (long)unix_seconds, 0, 0);
}

long sys_audio_claim(void) {
    return do_syscall(SYS_audio_claim, 0, 0, 0);
}

long sys_audio_release(void) {
    return do_syscall(SYS_audio_release, 0, 0, 0);
}

long sys_beep(uint32_t freq_hz, uint32_t ms) {
    return do_syscall(SYS_beep, (long)freq_hz, (long)ms, 0);
}

long sys_audio_volume(uint32_t percent) {
    return do_syscall(SYS_audio_volume, (long)percent, 0, 0);
}

long sys_audio_play(const int16_t *samples, uint32_t frames) {
    return do_syscall(SYS_audio_play, (long)samples, (long)frames, 0);
}

long sys_mouse_read(mouse_event_t *out) {
    return do_syscall(SYS_mouse_read, (long)out, 0, 0);
}

long sys_pipe_open(const char *name, int fds_out[2]) {
    return do_syscall(SYS_pipe_open, (long)name, (long)fds_out, 0);
}

long sys_kbd_read(char *out) {
    return do_syscall(SYS_kbd_read, (long)out, 0, 0);
}

long sys_pipe_poll(int fd) {
    return do_syscall(SYS_pipe_poll, fd, 0, 0);
}

long sys_uptime_ms(void) {
    return do_syscall(SYS_uptime_ms, 0, 0, 0);
}

/* M70 */
long sys_klog(uint64_t from, char *buf, size_t max, uint64_t *next_out) {
    return do_syscall6(SYS_klog, (long)from, (long)buf, (long)max, (long)next_out, 0, 0);
}

long sys_klog_total(void) {
    return do_syscall(SYS_klog_total, 0, 0, 0);
}

/* M71 */
long sys_rename_replace(const char *old_path, const char *new_path) {
    return do_syscall(SYS_rename_replace, (long)old_path, (long)new_path, 0);
}

/* M68 */
long sys_waitfds(const int *fds, int count, int timeout_ms) {
    return do_syscall(SYS_waitfds, (uint64_t)fds, (uint64_t)count, (uint64_t)(long)timeout_ms);
}

long sys_idle_ticks(int cpu) {
    return do_syscall(SYS_idle_ticks, (uint64_t)cpu, 0, 0);
}

long sys_dup2(int oldfd, int newfd) {
    return do_syscall(SYS_dup2, oldfd, newfd, 0);
}

long sys_wait_nb(long pid) {
    return do_syscall(SYS_wait_nb, pid, 0, 0);
}

long sys_yield(void) {
    return do_syscall(SYS_yield, 0, 0, 0);
}

long sys_kbd_modifiers(void) {
    return do_syscall(SYS_kbd_modifiers, 0, 0, 0);
}

long sys_clipboard_set(const void *buf, size_t len) {
    return do_syscall(SYS_clipboard_set, (long)buf, (long)len, 0);
}

long sys_clipboard_get(void *buf, size_t maxlen) {
    return do_syscall(SYS_clipboard_get, (long)buf, (long)maxlen, 0);
}

long sys_writefile(const char *name, const void *buf, size_t len) {
    return do_syscall(SYS_writefile, (long)name, (long)buf, (long)len);
}

long sys_task_alive(long pid) {
    return do_syscall(SYS_task_alive, pid, 0, 0);
}

long sys_pipe_reset(int fd) {
    return do_syscall(SYS_pipe_reset, fd, 0, 0);
}

long sys_taskinfo(task_info_t *buf, long max_entries) {
    return do_syscall(SYS_taskinfo, (long)buf, max_entries, 0);
}

long sys_profile(long op, void *arg, long count) {
    return do_syscall(SYS_profile, op, (long)arg, count);
}

long sys_shutdown(int mode) {
    return do_syscall(SYS_shutdown, mode, 0, 0);
}

long sys_close(int fd) {
    return do_syscall(SYS_close, fd, 0, 0);
}

long sys_shm_free(long id, void *vaddr) {
    return do_syscall(SYS_shm_free, id, (long)vaddr, 0);
}
