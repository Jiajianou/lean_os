#include "syscall_wrappers.h"

#include "syscall.h"

static long do_syscall(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("int $0x80"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r8", "r9", "memory");
    return ret;
}

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

long sys_write(int fd, const void *buffer, size_t length) {
    return do_syscall(SYS_write, fd, (long)buffer, (long)length);
}

void sys_exit(int code) {
    do_syscall(SYS_exit, code, 0, 0);
    for (;;) {
    }
}

long sys_getpid(void) {
    return do_syscall(SYS_getpid, 0, 0, 0);
}

long sys_spawnve(const char *path, const char *const *argv, const char *const *envp) {
    return do_syscall(SYS_spawn, (long)path, (long)argv, (long)envp);
}

long sys_spawnv(const char *path, const char *const *argv) {
    extern char **environ;
    return sys_spawnve(path, argv, (const char *const *)environ);
}

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

long sys_wireless(int operation, void *argument, unsigned long count) {
    return do_syscall(SYS_wireless, (uint64_t)operation, (uint64_t)argument, count);
}

long sys_thread_detach(long thread_id) {
    return do_syscall(SYS_thread_detach, (uint64_t)thread_id, 0, 0);
}

void sys_thread_exit(int value) {
    do_syscall(SYS_thread_exit, value, 0, 0);
    for (;;) {
    }
}

long sys_gettid(void) {
    return do_syscall(SYS_gettid, 0, 0, 0);
}

long sys_mmap(void *address, unsigned long length, int prot, int flags, int fd,
              unsigned long offset) {
    return do_syscall6(SYS_mmap, (long)address, (long)length, prot, flags, fd, (long)offset);
}

long sys_link(const char *old_path, const char *new_path) {
    return do_syscall(SYS_link, (long)old_path, (long)new_path, 0);
}

long sys_fsync(int fd) {
    return do_syscall(SYS_fsync, fd, 0, 0);
}

long sys_mprotect(void *address, unsigned long length, int prot) {
    return do_syscall(SYS_mprotect, (long)address, (long)length, prot);
}

long sys_madvise(void *address, unsigned long length, int advice) {
    return do_syscall(SYS_madvise, (long)address, (long)length, advice);
}

long sys_mincore(void *address, unsigned long length, unsigned char *vector) {
    return do_syscall(SYS_mincore, (long)address, (long)length, (long)vector);
}

long sys_sockname(int fd, os_sockaddr_t *out) {
    return do_syscall(SYS_sockname, (long)fd, (long)out, 0);
}

long sys_getrlimit(int resource, void *out) {
    return do_syscall(SYS_getrlimit, resource, (long)out, 0);
}

long sys_setrlimit(int resource, const void *in) {
    return do_syscall(SYS_setrlimit, resource, (long)in, 0);
}

long sys_thread_setname(const char *name) {
    return do_syscall(SYS_thread_setname, (long)name, 0, 0);
}

long sys_thread_getname(char *out, unsigned long length) {
    return do_syscall(SYS_thread_getname, (long)out, (long)length, 0);
}

long sys_munmap(void *address, unsigned long length) {
    return do_syscall(SYS_munmap, (long)address, (long)length, 0);
}

long sys_chdir(const char *path) {
    return do_syscall(SYS_chdir, (long)path, 0, 0);
}

long sys_getcwd(char *buffer, size_t maxlen) {
    return do_syscall(SYS_getcwd, (long)buffer, (long)maxlen, 0);
}

long sys_spawn(const char *path, const char *arg) {
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

long sys_read(int fd, void *buffer, size_t length) {
    return do_syscall(SYS_read, fd, (long)buffer, (long)length);
}

long sys_readfile(const char *name, void *buffer, size_t maxlen) {
    return do_syscall(SYS_readfile, (long)name, (long)buffer, (long)maxlen);
}

long sys_listdir(const char *path, void *buffer, size_t maxlen) {
    return do_syscall(SYS_listdir, (long)path, (long)buffer, (long)maxlen);
}

long sys_getdents(const char *path, unsigned int *cookie, void *buffer, size_t buflen) {
    return do_syscall6(SYS_getdents, (long)path, (long)cookie, (long)buffer,
                       (long)buflen, 0, 0);
}

long sys_fork(void) {
    return do_syscall(SYS_fork, 0, 0, 0);
}

long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    return do_syscall(SYS_execve, (long)path, (long)argv, (long)envp);
}

long sys_waitpid(long pid, int *status, long options) {
    return do_syscall6(SYS_waitpid, pid, (long)status, options, 0, 0, 0);
}

long sys_fcntl(int fd, int command, long arg) {
    return do_syscall(SYS_fcntl, fd, command, arg);
}

long sys_setpgid(long pid, long pgid) {
    return do_syscall(SYS_setpgid, pid, pgid, 0);
}

long sys_setsid(void) {
    return do_syscall(SYS_setsid, 0, 0, 0);
}

long sys_getsid(long pid) {
    return do_syscall(SYS_getsid, pid, 0, 0);
}

long sys_ioctl(int fd, unsigned long command, void *arg) {
    return do_syscall(SYS_ioctl, fd, (long)command, (long)arg);
}

long sys_ftruncate(int fd, long length) {
    return do_syscall(SYS_ftruncate, fd, length, 0);
}

long sys_symlink(const char *target, const char *path) {
    return do_syscall(SYS_symlink, (long)target, (long)path, 0);
}

long sys_readlink(const char *path, char *buffer, size_t length) {
    return do_syscall(SYS_readlink, (long)path, (long)buffer, (long)length);
}

long sys_lstat(const char *path, void *out) {
    return do_syscall(SYS_lstat, (long)path, (long)out, 0);
}

long sys_rusage(int who, void *out) {
    return do_syscall(SYS_rusage, who, (long)out, 0);
}

long sys_statvfs(const char *path, void *out) {
    return do_syscall(SYS_statvfs, (long)path, (long)out, 0);
}

long sys_fdpath(int fd, char *out, unsigned long out_length) {
    return do_syscall(SYS_fdpath, fd, (long)out, (long)out_length);
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

long sys_msync(void *address, unsigned long length, int flags) {
    return do_syscall(SYS_msync, (long)address, (long)length, flags);
}

long sys_arch_prctl(int code, unsigned long address) {
    return do_syscall(SYS_arch_prctl, code, (long)address, 0);
}

long sys_futex(volatile unsigned int *address, int op, unsigned int val,
               unsigned int timeout_ms) {
    return do_syscall6(SYS_futex, (long)address, op, (long)val, (long)timeout_ms, 0, 0);
}

long sys_getrandom(void *buffer, unsigned long length, unsigned int flags) {
    return do_syscall(SYS_getrandom, (uint64_t)buffer, length, flags);
}

long sys_pread(int fd, void *buffer, unsigned long length, long offset) {
    return do_syscall6(SYS_pread, (long)fd, (long)buffer, (long)length, offset, 0, 0);
}

long sys_pwrite(int fd, const void *buffer, unsigned long length, long offset) {
    return do_syscall6(SYS_pwrite, (long)fd, (long)buffer, (long)length, offset, 0, 0);
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

long sys_shared_memory_unmap(void *vaddr, unsigned long bytes) {
    return do_syscall(SYS_shared_memory_unmap, (long)vaddr, (long)bytes, 0);
}

long sys_kill(long pid, int sig) {
    return do_syscall(SYS_kill, pid, sig, 0);
}

long sys_pipe(int file_descriptors_out[2]) {
    return do_syscall(SYS_pipe, (long)file_descriptors_out, 0, 0);
}

long sys_getpgid(long pid) {
    return do_syscall(SYS_getpgid, pid, 0, 0);
}

long sys_sbrk(long increment) {
    return do_syscall(SYS_sbrk, increment, 0, 0);
}

long sys_shared_memory_create(size_t size) {
    return do_syscall(SYS_shared_memory_create, (long)size, 0, 0);
}

long sys_shared_memory_map(long id) {
    return do_syscall(SYS_shared_memory_map, id, 0, 0);
}

long sys_framebuffer_info(window_manager_framebuffer_info_t *out) {
    return do_syscall(SYS_framebuffer_info, (long)out, 0, 0);
}

long sys_framebuffer_info_physical(window_manager_framebuffer_info_t *out) {
    return do_syscall(SYS_framebuffer_info, (long)out, WINDOW_MANAGER_FRAMEBUFFER_PHYSICAL, 0);
}

long sys_framebuffer_map(void) {
    return do_syscall(SYS_framebuffer_map, 0, 0, 0);
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

long sys_getcaps(void) {
    return do_syscall(SYS_getcaps, 0, 0, 0);
}

long sys_dropcaps(uint32_t keep) {
    return do_syscall(SYS_dropcaps, (long)keep, 0, 0);
}

long sys_socket(int type) {
    return do_syscall(SYS_socket, type, OS_AF_INET, 0);
}

long sys_socket_in(int type, int domain) {
    return do_syscall(SYS_socket, type, domain, 0);
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

long sys_send(int fd, const void *data, uint32_t length) {
    return do_syscall(SYS_send, fd, (long)data, (long)length);
}

long sys_receive(int fd, void *data, uint32_t max) {
    return do_syscall(SYS_receive, fd, (long)data, (long)max);
}

long sys_peek(int fd, void *data, uint32_t max, int dontwait) {
    return do_syscall6(SYS_peek, fd, (long)data, (long)max, dontwait, 0, 0);
}

long sys_bind(int fd, uint16_t port) {
    return do_syscall(SYS_bind, fd, port, 0);
}

long sys_sendto(int fd, uint32_t ip, uint16_t port, const void *data, uint32_t length) {
    return do_syscall6(SYS_sendto, fd, (long)ip, port, (long)data, (long)length, 0);
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

long sys_socketpair(int type, int file_descriptors_out[2]) {
    return do_syscall(SYS_socketpair, type, (long)file_descriptors_out, 0);
}

long sys_unix_peer_credentials(int fd, void *out) {
    return do_syscall(SYS_unix_peer_credentials, fd, (long)out, 0);
}

long sys_sigaltstack(const void *new_stack, void *old_stack) {
    return do_syscall(SYS_sigaltstack, (long)new_stack, (long)old_stack, 0);
}

long sys_bindun(int fd, const char *name, int length) {
    return do_syscall(SYS_bindun, fd, (long)name, length);
}

long sys_connectun(int fd, const char *name, int length) {
    return do_syscall(SYS_connectun, fd, (long)name, length);
}

long sys_sendmsg(int fd, const os_message_t *message, int flags) {
    return do_syscall(SYS_sendmsg, fd, (long)message, flags);
}

long sys_recvmsg(int fd, os_message_t *message, int flags) {
    return do_syscall(SYS_recvmsg, fd, (long)message, flags);
}

long sys_sockshut(int fd, int how) {
    return do_syscall(SYS_sockshut, fd, how, 0);
}

long sys_epoll_create(int flags) {
    return do_syscall(SYS_epoll_create, flags, 0, 0);
}

long sys_epoll_control(int epfd, int op, int fd, const os_epoll_event_t *ev) {
    return do_syscall6(SYS_epoll_control, epfd, op, fd, (long)ev, 0, 0);
}

long sys_epoll_wait(int epfd, os_epoll_event_t *out, int maxevents, int timeout_ms) {
    return do_syscall6(SYS_epoll_wait, epfd, (long)out, maxevents, timeout_ms, 0, 0);
}

long sys_eventfd(uint64_t initval, int flags) {
    return do_syscall(SYS_eventfd, (long)initval, flags, 0);
}

long sys_timerfd_create(int clockid, int flags) {
    return do_syscall(SYS_timerfd_create, clockid, flags, 0);
}

long sys_timerfd_settime(int fd, int flags, const os_itimer_t *value, os_itimer_t *old) {
    return do_syscall6(SYS_timerfd_settime, fd, flags, (long)value, (long)old, 0, 0);
}

long sys_timerfd_gettime(int fd, os_itimer_t *out) {
    return do_syscall(SYS_timerfd_gettime, fd, (long)out, 0);
}

long sys_memfd_create(const char *name, int flags) {
    return do_syscall(SYS_memfd_create, (long)name, flags, 0);
}

long sys_memfd_seal(int fd, uint32_t add) {
    return do_syscall(SYS_memfd_seal, fd, (long)add, 0);
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

long sys_pipe_open(const char *name, int file_descriptors_out[2]) {
    return do_syscall(SYS_pipe_open, (long)name, (long)file_descriptors_out, 0);
}

long sys_keyboard_read(char *out) {
    return do_syscall(SYS_keyboard_read, (long)out, 0, 0);
}

long sys_pipe_poll(int fd) {
    return do_syscall(SYS_pipe_poll, fd, 0, 0);
}

long sys_uptime_ms(void) {
    return do_syscall(SYS_uptime_ms, 0, 0, 0);
}

long sys_clock_ns(void) {
    return do_syscall(SYS_clock_ns, 0, 0, 0);
}

long sys_kernel_log(uint64_t from, char *buffer, size_t max, uint64_t *next_out) {
    return do_syscall6(SYS_kernel_log, (long)from, (long)buffer, (long)max, (long)next_out, 0, 0);
}

long sys_kernel_log_total(void) {
    return do_syscall(SYS_kernel_log_total, 0, 0, 0);
}

long sys_rename_replace(const char *old_path, const char *new_path) {
    return do_syscall(SYS_rename_replace, (long)old_path, (long)new_path, 0);
}

long sys_waitfds(const int *file_descriptors, int count, int timeout_ms) {
    return do_syscall(SYS_waitfds, (uint64_t)file_descriptors, (uint64_t)count, (uint64_t)(long)timeout_ms);
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

long sys_keyboard_modifiers(void) {
    return do_syscall(SYS_keyboard_modifiers, 0, 0, 0);
}

long sys_clipboard_set(const void *buffer, size_t length) {
    return do_syscall(SYS_clipboard_set, (long)buffer, (long)length, 0);
}

long sys_clipboard_get(void *buffer, size_t maxlen) {
    return do_syscall(SYS_clipboard_get, (long)buffer, (long)maxlen, 0);
}

long sys_writefile(const char *name, const void *buffer, size_t length) {
    return do_syscall(SYS_writefile, (long)name, (long)buffer, (long)length);
}

long sys_task_alive(long pid) {
    return do_syscall(SYS_task_alive, pid, 0, 0);
}

long sys_pipe_reset(int fd) {
    return do_syscall(SYS_pipe_reset, fd, 0, 0);
}

long sys_taskinfo(task_info_t *buffer, long max_entries) {
    return do_syscall(SYS_taskinfo, (long)buffer, max_entries, 0);
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

long sys_shared_memory_free(long id, void *vaddr) {
    return do_syscall(SYS_shared_memory_free, id, (long)vaddr, 0);
}
