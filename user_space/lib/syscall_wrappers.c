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

long sys_spawnv(const char *path, const char *const *argv) {
    return do_syscall(SYS_spawn, (long)path, (long)argv, 0);
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

long sys_shutdown(int mode) {
    return do_syscall(SYS_shutdown, mode, 0, 0);
}

long sys_close(int fd) {
    return do_syscall(SYS_close, fd, 0, 0);
}

long sys_shm_free(long id, void *vaddr) {
    return do_syscall(SYS_shm_free, id, (long)vaddr, 0);
}
