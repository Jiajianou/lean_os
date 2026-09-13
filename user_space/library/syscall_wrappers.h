#pragma once

#include <stddef.h>

#include "display.h"
#include "os_time.h"
#include "os_net.h"
#include "os_poll.h"
#include "caps.h"
#include "syscall.h"
#include "input.h"
#include "power_mode.h"
#include "proc.h"
#include "wm.h"

long sys_raw(long num, long a1, long a2, long a3);

long sys_write(int fd, const void *buf, size_t len);
void sys_exit(int code) __attribute__((noreturn));
long sys_getpid(void);

long sys_spawn(const char *path, const char *arg);

long sys_spawnv(const char *path, const char *const *argv);

long sys_spawnve(const char *path, const char *const *argv, const char *const *envp);

long sys_thread_create(void *entry, void *arg, unsigned long stack_top);
void sys_thread_exit(int value) __attribute__((noreturn));
long sys_gettid(void);

long sys_mmap(void *addr, unsigned long len, int prot, int flags, int fd,
              unsigned long offset);
long sys_munmap(void *addr, unsigned long len);
long sys_link(const char *old_path, const char *new_path);
long sys_fsync(int fd);
long sys_mprotect(void *addr, unsigned long len, int prot);
long sys_madvise(void *addr, unsigned long len, int advice);

long sys_chdir(const char *path);
long sys_getcwd(char *buf, size_t maxlen);

long sys_sigaction(int signo, void *handler, void (*restorer)(void),
                   unsigned int flags);
long sys_sigprocmask(int how, unsigned int mask, unsigned int *old_out);
long sys_wait(long pid);

long sys_read(int fd, void *buf, size_t len);

long sys_readfile(const char *name, void *buf, size_t maxlen);

long sys_listdir(const char *path, void *buf, size_t maxlen);
long sys_getdents(const char *path, unsigned int *cookie, void *buf, size_t buflen);
long sys_fork(void);
long sys_execve(const char *path, char *const argv[], char *const envp[]);
long sys_waitpid(long pid, int *status, long options);
long sys_fcntl(int fd, int cmd, long arg);
long sys_setpgid(long pid, long pgid);
long sys_setsid(void);
long sys_getsid(long pid);
long sys_ioctl(int fd, unsigned long cmd, void *arg);
long sys_ftruncate(int fd, long length);
long sys_symlink(const char *target, const char *path);
long sys_readlink(const char *path, char *buf, size_t len);
long sys_lstat(const char *path, void *out);

long sys_rusage(int who, void *out);
long sys_statvfs(const char *path, void *out);
long sys_utime(const char *path, unsigned int mtime);
long sys_fdpath(int fd, char *out, unsigned long out_len);
long sys_getppid(void);
long sys_sync(void);
long sys_meminfo(void *out);
long sys_alarm(unsigned int seconds);
long sys_msync(void *addr, unsigned long len, int flags);
long sys_arch_prctl(int code, unsigned long addr);
long sys_futex(volatile unsigned int *addr, int op, unsigned int val,
               unsigned int timeout_ms);
long sys_getrandom(void *buf, unsigned long len, unsigned int flags);
long sys_pread(int fd, void *buf, unsigned long len, long offset);
long sys_pwrite(int fd, const void *buf, unsigned long len, long offset);

long sys_mkdir(const char *path);

long sys_unlink(const char *path);

long sys_rename(const char *old_path, const char *new_path);

long sys_shared_memory_unmap(void *vaddr, unsigned long bytes);

long sys_kill(long pid, int sig);

long sys_pipe(int file_descriptors_out[2]);

long sys_getpgid(long pid);

long sys_sbrk(long increment);

long sys_shared_memory_create(size_t size);

long sys_shared_memory_map(long id);

long sys_framebuffer_info(wm_fb_info_t *out);

long sys_framebuffer_map(void);

long sys_display_modes(display_mode_t *out, long max);

long sys_display_set_mode(uint32_t width, uint32_t height);

long sys_open(const char *path, uint32_t flags);
long sys_lseek(int fd, long offset, int whence);
long sys_stat(const char *path, os_stat_t *out);

long sys_fstat(int fd, os_stat_t *out);
long sys_rmdir(const char *path);

long sys_time(os_datetime_t *out);

long sys_getcaps(void);
long sys_dropcaps(uint32_t keep);

long sys_socket(int type);
long sys_socket_in(int type, int domain);

long sys_listen(int fd);
long sys_connect(int fd, uint32_t ip, uint16_t port);
long sys_connstat(int fd);
long sys_accept(int fd, os_sockaddr_t *from);
long sys_send(int fd, const void *data, uint32_t len);
long sys_recv(int fd, void *data, uint32_t max);
long sys_bind(int fd, uint16_t port);
long sys_sendto(int fd, uint32_t ip, uint16_t port, const void *data, uint32_t len);
long sys_recvfrom(int fd, void *data, uint32_t max, os_sockaddr_t *from);
long sys_sockpoll(int fd);
long sys_netconf(os_netconf_t *out);

long sys_socketpair(int type, int file_descriptors_out[2]);
long sys_bindun(int fd, const char *name, int len);
long sys_connectun(int fd, const char *name, int len);
long sys_sendmsg(int fd, const os_msg_t *msg, int flags);
long sys_recvmsg(int fd, os_msg_t *msg, int flags);
long sys_sockshut(int fd, int how);

long sys_epoll_create(int flags);
long sys_epoll_ctl(int epfd, int op, int fd, const os_epoll_event_t *ev);
long sys_epoll_wait(int epfd, os_epoll_event_t *out, int maxevents, int timeout_ms);
long sys_eventfd(uint64_t initval, int flags);
long sys_timerfd_create(int clockid, int flags);
long sys_timerfd_settime(int fd, int flags, const os_itimer_t *value, os_itimer_t *old);
long sys_timerfd_gettime(int fd, os_itimer_t *out);

long sys_memfd_create(const char *name, int flags);
long sys_memfd_seal(int fd, uint32_t add);

long sys_settime(uint32_t unix_seconds);

long sys_audio_claim(void);
long sys_audio_release(void);
long sys_beep(uint32_t freq_hz, uint32_t ms);
long sys_audio_volume(uint32_t percent);
long sys_audio_play(const int16_t *samples, uint32_t frames);

long sys_mouse_read(mouse_event_t *out);

long sys_pipe_open(const char *name, int file_descriptors_out[2]);

long sys_keyboard_read(char *out);

long sys_pipe_poll(int fd);

long sys_uptime_ms(void);

long sys_klog(uint64_t from, char *buf, size_t max, uint64_t *next_out);

long sys_klog_total(void);

long sys_rename_replace(const char *old_path, const char *new_path);
long sys_waitfds(const int *fds, int count, int timeout_ms);

long sys_idle_ticks(int cpu);

long sys_dup2(int oldfd, int newfd);

long sys_wait_nb(long pid);

long sys_yield(void);

long sys_task_alive(long pid);

long sys_pipe_reset(int fd);

long sys_keyboard_modifiers(void);

long sys_clipboard_set(const void *buf, size_t len);
long sys_clipboard_get(void *buf, size_t maxlen);

long sys_writefile(const char *name, const void *buf, size_t len);

long sys_taskinfo(task_info_t *buf, long max_entries);

long sys_profile(long op, void *arg, long count);

long sys_shutdown(int mode);

long sys_close(int fd);

long sys_shared_memory_free(long id, void *vaddr);
