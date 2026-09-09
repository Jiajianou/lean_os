/* tests/fakes/fake_user_fs.c - M112
 *
 * The five filesystem calls user_space/lib/fsutil.c makes, backed by the
 * host's own filesystem under a temporary directory.
 *
 * Why this shape rather than a RAM model of leanfs: the thing under test
 * is a *recursive delete*, and the only assertion worth making about one
 * is "is it gone" - asked of something that is not the code that deleted
 * it. A model filesystem written for this test would answer that
 * question with the same assumptions the walk was written under, which
 * is the failure the mutation harness exists to catch. The host's `stat`
 * has no such assumptions. It is the same argument the differential
 * tests make (tools/sh-test.sh and the five beside it): where an
 * independent implementation of the same thing exists, grade against it.
 *
 * Every path is rewritten to sit under a fresh mkdtemp, made per reset. A test that asks to delete "/" therefore
 * deletes the temp directory and nothing above it - the mapping is what
 * makes a test about recursive deletion safe to run on a developer's
 * machine, and it is why nothing here ever passes a caller's path
 * through unmodified.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fakes.h"

#include "os_time.h"   /* system_api/include/os_time.h - os_stat_t */

/* This ABI names its wait flags the same things POSIX does, and on the
 * host <sys/wait.h> has already defined them by the time this file
 * gets here (stdlib.h pulls it in). The values agree; only -Wmacro-
 * redefined objects. Dropped rather than renamed on either side: the
 * guest header is an ABI and the host header is the platform, and this
 * file wants neither of their wait flags - only os_dirent_t. */
#undef WNOHANG
#undef WUNTRACED
#include "syscall.h"   /* system_api/include/syscall.h - os_dirent_t */

static char fs_root[512];

void fake_user_fs_reset(void) {
    if (fs_root[0]) {
        /* Left behind on purpose if the removal fails: a test that
         * leaks a temp directory is a nuisance, and one that deletes
         * something it should not is a catastrophe, so this uses the
         * host's own rm -rf rather than a walk of its own. */
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fs_root);
        if (system(cmd) != 0) {
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

/* A guest path to a host one. Returns 0 on success, -1 if it would not
 * fit - which is a refusal, never a truncation, for the same reason
 * paths.h's path_join refuses. */
static int host_path(const char *guest, char *out, size_t out_len) {
    if (!fs_root[0] || !guest || guest[0] != '/') {
        return -1;
    }
    int n = snprintf(out, out_len, "%s%s", fs_root, guest);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
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

/* ---- the calls fsutil.c makes ---------------------------------------- */

long sys_stat(const char *path, os_stat_t *out) {
    char host[1024];
    struct stat st;
    if (host_path(path, host, sizeof(host)) != 0 || stat(host, &st) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->size = (uint32_t)st.st_size;
    out->mtime = (uint32_t)st.st_mtime;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->kind = S_ISDIR(st.st_mode) ? OS_STAT_DIR : OS_STAT_FILE;
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

/* The one call with real semantics to reproduce. SYS_getdents fills the
 * buffer with as many *whole* os_dirent_t records as fit and reports how
 * far it got in `cookie`; a partial record is never written, and 0 means
 * the end of the directory. `cookie` is opaque to the caller, so an
 * entry index is a legal implementation of it - and it is the one that
 * makes "read, delete, read again from zero" behave here exactly as it
 * does on the machine.
 *
 * Deliberately hands back at most FAKE_BATCH records per call even when
 * more would fit, because the batching is the part of the contract
 * fsutil.c's re-read loop exists to survive: a fake that always returned
 * a whole directory would never once exercise it. */
#define FAKE_BATCH 3

long sys_getdents(const char *path, unsigned int *cookie, void *buf, size_t buflen) {
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
            continue; /* leanfs stores neither, so neither is reported */
        }
        if (index++ < *cookie) {
            continue;
        }
        size_t name_len = strlen(e->d_name);
        size_t reclen = 8 + name_len + 1;
        reclen = (reclen + 7u) & ~(size_t)7u;
        if (used + reclen > buflen || written >= FAKE_BATCH) {
            break;
        }
        char child[2048];
        struct stat st;
        snprintf(child, sizeof(child), "%s/%s", host, e->d_name);
        int is_dir = (stat(child, &st) == 0) && S_ISDIR(st.st_mode);

        os_dirent_t *rec = (os_dirent_t *)(void *)((char *)buf + used);
        rec->ino = (unsigned int)index;
        rec->reclen = (unsigned short)reclen;
        rec->type = is_dir ? OS_DT_DIR : OS_DT_REG;
        rec->name_len = (unsigned char)name_len;
        memcpy(rec->name, e->d_name, name_len + 1);
        used += reclen;
        written++;
        *cookie = index;
    }
    closedir(d);
    return (long)used;
}
