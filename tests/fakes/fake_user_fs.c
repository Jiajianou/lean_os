#include <dirent.h>
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
        int is_directory = (stat(child, &st) == 0) && S_ISDIR(st.st_mode);

        os_dirent_t *rec = (os_dirent_t *)(void *)((char *)buffer + used);
        rec->ino = (unsigned int)index;
        rec->reclen = (unsigned short)reclen;
        rec->type = is_directory ? OS_DT_DIRECTORY : OS_DT_REG;
        rec->name_length = (unsigned char)name_length;
        memcpy(rec->name, e->d_name, name_length + 1);
        used += reclen;
        written++;
        *cookie = index;
    }
    closedir(d);
    return (long)used;
}
