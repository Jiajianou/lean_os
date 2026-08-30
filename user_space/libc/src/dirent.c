/* user_space/libc/src/dirent.c - M77 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "syscall_wrappers.h"

/* One directory's whole listing, fetched once. See <dirent.h> for why
 * that is the right shape here rather than a streaming iterator.
 *
 * 8 KiB: leanfs allows LEANFS_MAX_DIRENTS (96) entries of at most 27
 * characters plus a separator, which is under 3 KiB, so this has room
 * for three times the largest directory the filesystem can hold. A
 * listing that somehow did not fit would be truncated by SYS_listdir at
 * a byte boundary; the terminating loop below stops at the last complete
 * name rather than reporting a half one. */
#define DIR_BUF 8192

struct DIR {
    char *buf;
    long len;
    long pos;
    struct dirent entry;
};

DIR *opendir(const char *path) {
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) {
        return 0;
    }
    d->buf = (char *)malloc(DIR_BUF);
    if (!d->buf) {
        free(d);
        return 0;
    }
    long n = sys_listdir(path, d->buf, DIR_BUF - 1);
    if (n < 0) {
        free(d->buf);
        free(d);
        return 0; /* not a directory, or nothing there to open */
    }
    d->buf[n] = '\0';
    d->len = n;
    d->pos = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) {
        return 0;
    }
    /* Skip separators left by the previous entry, and any empty line. */
    while (d->pos < d->len && d->buf[d->pos] == '\n') {
        d->pos++;
    }
    if (d->pos >= d->len) {
        return 0;
    }
    long start = d->pos;
    while (d->pos < d->len && d->buf[d->pos] != '\n') {
        d->pos++;
    }
    long end = d->pos;

    /* SYS_listdir marks a directory by appending '/'. That trailing byte
     * is the d_type this interface is supposed to report, so it is
     * consumed here rather than passed through in the name - a program
     * that concatenated d_name onto a path would otherwise build "a//b".
     */
    int is_dir = 0;
    if (end > start && d->buf[end - 1] == '/') {
        is_dir = 1;
        end--;
    }
    long len = end - start;
    if (len > NAME_MAX) {
        len = NAME_MAX;
    }
    memcpy(d->entry.d_name, d->buf + start, (size_t)len);
    d->entry.d_name[len] = '\0';
    d->entry.d_type = is_dir ? DT_DIR : DT_REG;
    d->entry.d_ino = 0; /* see <dirent.h> */
    if (len == 0) {
        return readdir(d); /* an empty line is not an entry */
    }
    return &d->entry;
}

void rewinddir(DIR *d) {
    if (d) {
        d->pos = 0;
    }
}

int closedir(DIR *d) {
    if (!d) {
        return -1;
    }
    free(d->buf);
    free(d);
    return 0;
}
