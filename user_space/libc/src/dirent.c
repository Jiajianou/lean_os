/* user_space/libc/src/dirent.c - M77, rewritten in M81
 *
 * M77 built this over SYS_listdir, which hands back a directory's whole
 * listing as newline-separated names in one buffer. <dirent.h>'s own
 * header comment defended that as "a real design choice and not a
 * shortcut", on the grounds that "a directory here holds at most
 * LEANFS_MAX_DIRENTS entries, which is kilobytes".
 *
 * M81 took that ceiling from 192 entries of at most 27 characters to
 * 8192 of at most 255, which is two megabytes - so the argument that
 * made whole-shot right is exactly the one that stopped being true. The
 * defence was honest about what it depended on, and the dependency
 * moved.
 *
 * So this reads through SYS_getdents a bufferful at a time. The buffer is
 * a fixed size chosen once, not a size derived from the directory,
 * because "how big is this directory" is a question the caller no longer
 * has to answer to walk one - which is the whole point.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "paths.h"   /* system_api/include/paths.h  - PATH_MAX_LEN */
#include "syscall.h" /* system_api/include/syscall.h - os_dirent_t, OS_DIRENT_MAX */
#include "syscall_wrappers.h"

/* One fetch's worth of records. Sized so a directory of ordinary names
 * comes back in a handful of syscalls (about 120 twenty-character names
 * per fetch) while still being comfortably bigger than OS_DIRENT_MAX, so
 * a single very long name can never fail to fit. */
#define DIR_BUF 4096

_Static_assert(DIR_BUF >= OS_DIRENT_MAX, "a fetch buffer must hold the longest single record");

struct DIR {
    unsigned int cookie; /* the kernel's position; 0 is the start */
    long len;            /* bytes of valid records in buf */
    long pos;            /* how far through buf readdir has walked */
    int at_end;          /* the kernel has reported end-of-directory */
    char path[PATH_MAX_LEN];
    struct dirent entry;
    char buf[DIR_BUF] __attribute__((aligned(8)));
};

DIR *opendir(const char *path) {
    if (!path) {
        return 0;
    }
    size_t plen = strlen(path);
    if (plen >= PATH_MAX_LEN) {
        return 0;
    }

    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) {
        return 0;
    }
    memcpy(d->path, path, plen + 1);
    d->cookie = 0;
    d->len = 0;
    d->pos = 0;
    d->at_end = 0;

    /* One fetch here, so that opendir on something that is not a
     * directory fails at opendir rather than at the first readdir - which
     * is the contract every caller written elsewhere expects, and the one
     * M77's whole-shot version gave for free. */
    long n = sys_getdents(d->path, &d->cookie, d->buf, sizeof(d->buf));
    if (n < 0) {
        free(d);
        return 0;
    }
    d->len = n;
    d->at_end = (n == 0);
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) {
        return 0;
    }
    for (;;) {
        if (d->pos < d->len) {
            const os_dirent_t *r = (const os_dirent_t *)(const void *)(d->buf + d->pos);
            /* A record that claims to run past what the kernel said it
             * wrote is not something to walk into. This cannot happen
             * across the syscall boundary as written, and checking is
             * four instructions against a walk that would otherwise run
             * off the end of the buffer. */
            if (r->reclen < sizeof(os_dirent_t) || d->pos + r->reclen > d->len) {
                return 0;
            }
            size_t n = r->name_len;
            if (n > NAME_MAX) {
                n = NAME_MAX;
            }
            memcpy(d->entry.d_name, r->name, n);
            d->entry.d_name[n] = '\0';
            d->entry.d_type = r->type;
            d->entry.d_ino = r->ino; /* M81: a real inode number - see <dirent.h> */
            d->pos += r->reclen;
            return &d->entry;
        }
        if (d->at_end) {
            return 0;
        }
        /* This bufferful is spent; ask for the next one. */
        long n = sys_getdents(d->path, &d->cookie, d->buf, sizeof(d->buf));
        if (n <= 0) {
            d->at_end = 1;
            return 0;
        }
        d->len = n;
        d->pos = 0;
    }
}

void rewinddir(DIR *d) {
    if (!d) {
        return;
    }
    /* Back to the kernel's own start-of-directory rather than to the
     * start of the buffer: the buffer holds one fetch out of what may be
     * many, so rewinding to `pos = 0` would have replayed the middle of
     * the directory and called it the beginning. */
    d->cookie = 0;
    d->len = 0;
    d->pos = 0;
    d->at_end = 0;
    long n = sys_getdents(d->path, &d->cookie, d->buf, sizeof(d->buf));
    if (n > 0) {
        d->len = n;
    } else {
        d->at_end = 1;
    }
}

int closedir(DIR *d) {
    if (!d) {
        return -1;
    }
    free(d);
    return 0;
}
