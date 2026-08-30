/* user_space/libc/src/unistd.c - M75
 *
 * <unistd.h>'s implementations. Every one is a thin renaming of a
 * syscall wrapper that already existed, which is the whole point: a
 * program somebody else wrote calls `chdir`, `write` and `getcwd`, not
 * `sys_chdir`, `sys_write` and `sys_getcwd`, and the gap between those
 * two spellings is the only thing that was stopping it.
 */
#include <unistd.h>

#include "syscall_wrappers.h"

int chdir(const char *path) {
    return (int)sys_chdir(path);
}

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) {
        return 0;
    }
    /* Refused rather than truncated by the kernel, so a NULL here means
     * "your buffer is too small", which is exactly what a caller has to
     * know in order to try a bigger one. */
    if (sys_getcwd(buf, size) < 0) {
        return 0;
    }
    return buf;
}

int getpid(void) {
    return (int)sys_getpid();
}

long read(int fd, void *buf, size_t count) {
    return sys_read(fd, buf, count);
}

long write(int fd, const void *buf, size_t count) {
    return sys_write(fd, buf, count);
}

int close(int fd) {
    return (int)sys_close(fd);
}

int access(const char *path, int mode) {
    os_stat_t st;
    if (sys_stat(path, &st) != 0) {
        return -1;
    }
    /* Existence is the only question this machine can answer honestly -
     * see the note in <unistd.h>. Anything that exists is reachable, and
     * whether an operation is permitted is decided by the caller's
     * capability set at the moment it tries. */
    (void)mode;
    return 0;
}

int rmdir(const char *path) {
    return (int)sys_rmdir(path);
}

int unlink(const char *path) {
    return (int)sys_unlink(path);
}

long spawnv(const char *path, char *const argv[]) {
    return sys_spawnv(path, (const char *const *)argv);
}

long spawnve(const char *path, char *const argv[], char *const envp[]) {
    return sys_spawnve(path, (const char *const *)argv, (const char *const *)envp);
}
