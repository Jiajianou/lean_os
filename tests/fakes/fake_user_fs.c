#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fakes.h"

#include "os_time.h"

#undef WNOHANG
#undef WUNTRACED
#include "syscall.h"

static char fs_root[512];

void fake_user_fs_reset(void) {
    if (fs_root[0]) {
        char command[600];
        snprintf(command, sizeof(command), "rm -rf '%s'", fs_root);
        if (system(command) != 0) {
            fprintf(stderr, "fake_user_fs: could not clean %s\n", fs_root);
        }
        fs_root[0] = '\0';
    }
    char tmpl[] = "/tmp/leanos-fsutil-XXXXXX";
    const char *made = mkdtemp(tmpl);
    if (!made) {
        fprintf(stderr, "fake_user_fs: mkdtemp failed\n");
        abort();
    }
    snprintf(fs_root, sizeof(fs_root), "%s", made);
}

static int host_path(const char *guest, char *out, size_t out_length) {
    if (!fs_root[0] || !guest || guest[0] != '/') {
        return -1;
    }
    int n = snprintf(out, out_length, "%s%s", fs_root, guest);
    return (n < 0 || (size_t)n >= out_length) ? -1 : 0;
}

int fake_user_fs_mkdir(const char *guest) {
    char host[1024];
    if (host_path(guest, host, sizeof(host)) != 0) {
        return -1;
    }
    return mkdir(host, 0755);
}

int fake_user_fs_write(const char *guest, size_t bytes) {
    char host[1024];
    if (host_path(guest, host, sizeof(host)) != 0) {
        return -1;
    }
    FILE *f = fopen(host, "wb");
    if (!f) {
        return -1;
    }
    for (size_t i = 0; i < bytes; i++) {
        fputc('x', f);
    }
    fclose(f);
    return 0;
}

int fake_user_fs_exists(const char *guest) {
    char host[1024];
    struct stat st;
    if (host_path(guest, host, sizeof(host)) != 0) {
        return 0;
    }
    return stat(host, &st) == 0;
}

long sys_stat(const char *path, os_stat_t *out) {
    char host[1024];
    struct stat st;
    if (host_path(path, host, sizeof(host)) != 0 || stat(host, &st) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->size = (uint32_t)st.st_size;
    out->mtime = (uint32_t)st.st_mtime;
    out->is_directory = S_ISDIR(st.st_mode) ? 1 : 0;
    out->kind = S_ISDIR(st.st_mode) ? OS_STAT_DIRECTORY : OS_STAT_FILE;
    out->inode = (uint32_t)st.st_ino;
    return 0;
}

long sys_unlink(const char *path) {
    char host[1024];
    if (host_path(path, host, sizeof(host)) != 0) {
        return -1;
    }
    return unlink(host) == 0 ? 0 : -1;
}

long sys_rmdir(const char *path) {
    char host[1024];
    if (host_path(path, host, sizeof(host)) != 0) {
        return -1;
    }
    return rmdir(host) == 0 ? 0 : -1;
}

#define FAKE_BATCH 3

long sys_getdents(const char *path, unsigned int *cookie, void *buffer, size_t buflen) {
    char host[1024];
    if (host_path(path, host, sizeof(host)) != 0) {
        return -1;
    }
    DIR *d = opendir(host);
    if (!d) {
        return -1;
    }
    unsigned int index = 0;
    size_t used = 0;
    int written = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (index++ < *cookie) {
            continue;
        }
        size_t name_length = strlen(e->d_name);
        size_t reclen = 8 + name_length + 1;
        reclen = (reclen + 7u) & ~(size_t)7u;
        if (used + reclen > buflen || written >= FAKE_BATCH) {
            break;
        }
        char child[2048];
        struct stat st;
        snprintf(child, sizeof(child), "%s/%s", host, e->d_name);
        int found = lstat(child, &st) == 0;
        int is_directory = found && S_ISDIR(st.st_mode);
        int is_link = found && S_ISLNK(st.st_mode);

        os_dirent_t *rec = (os_dirent_t *)(void *)((char *)buffer + used);
        rec->ino = (unsigned int)index;
        rec->reclen = (unsigned short)reclen;
        rec->type = is_link ? OS_DT_LNK : is_directory ? OS_DT_DIRECTORY : OS_DT_REG;
        rec->name_length = (unsigned char)name_length;
        memcpy(rec->name, e->d_name, name_length + 1);
        used += reclen;
        written++;
        *cookie = index;
    }
    closedir(d);
    return (long)used;
}

#define FAKE_DESCRIPTOR_COUNT 32

static int fake_descriptors[FAKE_DESCRIPTOR_COUNT];
static int fake_descriptors_ready;

static void descriptors_ready(void) {
    if (!fake_descriptors_ready) {
        for (int i = 0; i < FAKE_DESCRIPTOR_COUNT; i++) {
            fake_descriptors[i] = -1;
        }
        fake_descriptors_ready = 1;
    }
}

int fake_user_fs_owns(int fd) {
    return fd >= FAKE_USER_FS_FIRST_DESCRIPTOR &&
           fd < FAKE_USER_FS_FIRST_DESCRIPTOR + FAKE_DESCRIPTOR_COUNT;
}

int fake_user_fs_open_count(void) {
    descriptors_ready();
    int open_count = 0;
    for (int i = 0; i < FAKE_DESCRIPTOR_COUNT; i++) {
        open_count += fake_descriptors[i] >= 0;
    }
    return open_count;
}

long sys_open(const char *path, uint32_t flags) {
    char host[1024];
    if (host_path(path, host, sizeof(host)) != 0) {
        return -1;
    }
    descriptors_ready();
    int mode = (flags & OPEN_WRITE) ? ((flags & OPEN_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & OPEN_CREATE) {
        mode |= O_CREAT;
    }
    if (flags & OPEN_EXCL) {
        mode |= O_EXCL;
    }
    if (flags & OPEN_TRUNCATE) {
        mode |= O_TRUNC;
    }
    if (flags & OPEN_APPEND) {
        mode |= O_APPEND;
    }
    for (int i = 0; i < FAKE_DESCRIPTOR_COUNT; i++) {
        if (fake_descriptors[i] < 0) {
            int real = open(host, mode, 0644);
            if (real < 0) {
                return -1;
            }
            fake_descriptors[i] = real;
            return FAKE_USER_FS_FIRST_DESCRIPTOR + i;
        }
    }
    return -1;
}

long fake_user_fs_read(int fd, void *buffer, size_t length) {
    descriptors_ready();
    int real = fake_descriptors[fd - FAKE_USER_FS_FIRST_DESCRIPTOR];
    return real < 0 ? -1 : (long)read(real, buffer, length);
}

long fake_user_fs_write_descriptor(int fd, const void *buffer, size_t length) {
    descriptors_ready();
    int real = fake_descriptors[fd - FAKE_USER_FS_FIRST_DESCRIPTOR];
    if (real < 0) {
        return -1;
    }
    long limit = fake_user_fs_write_limit;
    if (limit >= 0 && (long)length > limit) {
        length = (size_t)limit;
    }
    if (limit >= 0) {
        fake_user_fs_write_limit -= (long)length;
    }
    return (long)write(real, buffer, length);
}

long fake_user_fs_close(int fd) {
    descriptors_ready();
    int slot = fd - FAKE_USER_FS_FIRST_DESCRIPTOR;
    if (fake_descriptors[slot] < 0) {
        return -1;
    }
    close(fake_descriptors[slot]);
    fake_descriptors[slot] = -1;
    return 0;
}

long fake_user_fs_write_limit = -1;

long sys_mkdir(const char *path) {
    return fake_user_fs_mkdir(path) == 0 ? 0 : -1;
}

long sys_rename(const char *old_path, const char *new_path) {
    char from[1024];
    char to[1024];
    struct stat st;
    if (host_path(old_path, from, sizeof(from)) != 0 || host_path(new_path, to, sizeof(to)) != 0 ||
        lstat(to, &st) == 0) {
        return -1;
    }
    return rename(from, to) == 0 ? 0 : -1;
}

long sys_lstat(const char *path, void *out_pointer) {
    char host[1024];
    struct stat st;
    if (host_path(path, host, sizeof(host)) != 0 || lstat(host, &st) != 0) {
        return -1;
    }
    os_stat_t *out = (os_stat_t *)out_pointer;
    memset(out, 0, sizeof(*out));
    out->size = (uint32_t)st.st_size;
    out->mtime = (uint32_t)st.st_mtime;
    out->is_directory = S_ISDIR(st.st_mode) ? 1 : 0;
    out->is_link = S_ISLNK(st.st_mode) ? 1 : 0;
    out->inode = (uint32_t)st.st_ino;
    return 0;
}

long sys_readlink(const char *path, char *buffer, size_t length) {
    char host[1024];
    if (host_path(path, host, sizeof(host)) != 0) {
        return -1;
    }
    ssize_t n = readlink(host, buffer, length);
    return n < 0 ? -1 : (long)n;
}

long sys_symlink(const char *target, const char *path) {
    char host[1024];
    if (host_path(path, host, sizeof(host)) != 0) {
        return -1;
    }
    return symlink(target, host) == 0 ? 0 : -1;
}

int fake_user_fs_write_text(const char *guest, const char *text) {
    char host[1024];
    if (host_path(guest, host, sizeof(host)) != 0) {
        return -1;
    }
    FILE *f = fopen(host, "wb");
    if (!f) {
        return -1;
    }
    fputs(text, f);
    fclose(f);
    return 0;
}

int fake_user_fs_read_text(const char *guest, char *out, size_t capacity) {
    char host[1024];
    if (host_path(guest, host, sizeof(host)) != 0) {
        return -1;
    }
    FILE *f = fopen(host, "rb");
    if (!f) {
        return -1;
    }
    size_t n = fread(out, 1, capacity - 1, f);
    out[n] = '\0';
    fclose(f);
    return (int)n;
}

int fake_user_fs_symlink(const char *target, const char *guest) {
    return (int)sys_symlink(target, guest);
}
