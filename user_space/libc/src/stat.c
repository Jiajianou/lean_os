/* user_space/libc/src/stat.c - M77
 *
 * `struct stat` from SYS_stat's three fields, plus the constants a
 * program expects to be able to read. See <sys/stat.h> for what each
 * field is and, more importantly, which of them are zero on purpose.
 */
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "syscall_wrappers.h"

static void fill(struct stat *out, const os_stat_t *st) {
    memset(out, 0, sizeof(*out));
    /* M87: three types now, and the order matters - a link is checked
     * first because a link's own is_dir says nothing about the link. */
    out->st_mode = st->is_link ? S_IFLNK : (st->is_dir ? S_IFDIR : S_IFREG);
    out->st_size = (off_t)st->size;
    out->st_mtime = (time_t)st->mtime;
    out->st_atime = out->st_mtime; /* leanfs stores one timestamp, not three */
    out->st_ctime = out->st_mtime;
    out->st_nlink = 1;             /* no hard links exist here, so this is a fact rather than a default */
    out->st_blksize = 512;         /* leanfs's own block size (LEANFS_BLOCK_SIZE) */
    out->st_blocks = (blkcnt_t)((st->size + 511u) / 512u);
}

int stat(const char *path, struct stat *out) {
    os_stat_t st;
    if (!out || sys_stat(path, &st) != 0) {
        return -1;
    }
    fill(out, &st);
    return 0;
}

/* M87: a real lstat, over the syscall that does not follow a final link.
 *
 * This was an alias for `stat` for ten milestones, with an honest note
 * that "there are no symbolic links on this filesystem, so there is
 * nothing for lstat to decline to follow" - and a correct argument that
 * an alias beat a stub returning an error, because a tree walker calling
 * lstat is asking "what is this entry" and stat answered it. leanfs has
 * links now, so the two calls answer different questions and the alias
 * would give the wrong one: a walker following it would descend into
 * whatever a link pointed at, including a directory above itself. */
int lstat(const char *path, struct stat *out) {
    os_stat_t st;
    if (!out || sys_lstat(path, &st) != 0) {
        return -1;
    }
    fill(out, &st);
    return 0;
}

int fstat(int fd, struct stat *out) {
    os_stat_t st;
    if (!out || sys_fstat(fd, &st) != 0) {
        return -1;
    }
    fill(out, &st);
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode; /* accepted and ignored - see <sys/stat.h> */
    return (int)sys_mkdir(path);
}
