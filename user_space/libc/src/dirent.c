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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    /* M89: -1 for opendir, and the caller's descriptor for fdopendir -
     * which owns it and closes it in closedir. See fdopendir below. */
    int fd;
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
    d->fd = -1;
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
    /* fdopendir hands its descriptor to the DIR, and POSIX is explicit
     * that closedir closes it. A caller that keeps using the fd after
     * closedir is the bug this is on the right side of. */
    if (d->fd >= 0) {
        close(d->fd);
    }
    free(d);
    return 0;
}

/* ---- M89: a directory stream over a descriptor ----------------------
 *
 * toybox's tree walker opens each directory once and then wants to both
 * read it and use the descriptor as an *at() base, which is what this
 * call exists for.
 *
 * SYS_getdents takes a path rather than a descriptor - see the file
 * header for why the stream is built the way it is - so this asks the
 * kernel what path the descriptor names (SYS_fdpath) and opens a stream
 * on it. The result is a DIR that reads the right directory and a
 * descriptor that stays open and owned, which is the whole observable
 * contract. What it is not is a stream immune to the directory being
 * renamed underneath it; <fcntl.h> covers that, in the same terms and
 * for the same reason.
 */
DIR *fdopendir(int fd) {
    char path[PATH_MAX_LEN];
    if (fd < 0 || sys_fdpath(fd, path, sizeof(path)) < 0) {
        errno = EBADF;
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) {
        return 0;
    }
    d->fd = fd;
    return d;
}

int dirfd(DIR *d) {
    if (!d) {
        errno = EINVAL;
        return -1;
    }
    if (d->fd < 0) {
        /* opendir here does not hold a descriptor at all - the stream is
         * a path and a cookie. Reporting -1 is the truthful answer and
         * the one a caller can act on; inventing an open() to satisfy
         * the call would leak a descriptor nobody asked for. */
        errno = ENOTSUP;
        return -1;
    }
    return d->fd;
}

/* ---- M100: scandir and alphasort ------------------------------------
 *
 * NetSurf's file: fetcher is what named these - it is how a browser
 * turns a directory into a page you can click through. See <dirent.h>
 * for the ownership contract, which is the part of this interface worth
 * being careful about.
 *
 * Two things here are deliberate and neither is obvious:
 *
 * 1. **The failure path frees everything.** POSIX says the caller owns
 *    the array only on success, so an allocation that fails halfway
 *    through a large directory has to unwind what it already took. A
 *    scandir that returned -1 with entries still on the heap would leak
 *    exactly when memory is short, which is the worst time to leak.
 *
 * 2. **The sort is not a bare qsort.** POSIX's comparison function takes
 *    `const struct dirent **` and qsort's takes `const void *`. They are
 *    compatible in practice on this ABI and calling one through the
 *    other is still undefined behaviour, so this sorts through a
 *    trampoline that does the conversion honestly. The trampoline needs
 *    the caller's function, and qsort has no context argument (qsort_r
 *    is not in this libc), so it comes through a file-scope pointer -
 *    which is why the sort is NOT reentrant and says so here rather than
 *    leaving somebody to find out. Nothing on this machine sorts two
 *    directories at once; if something ever does, this needs an
 *    insertion sort of its own rather than a lock.
 */
static int (*scandir_cmp)(const struct dirent **, const struct dirent **);

static int scandir_trampoline(const void *a, const void *b) {
    const struct dirent *const *pa = a;
    const struct dirent *const *pb = b;
    return scandir_cmp((const struct dirent **)pa, (const struct dirent **)pb);
}

int scandir(const char *path, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    if (!path || !namelist) {
        errno = EINVAL;
        return -1;
    }
    DIR *d = opendir(path);
    if (!d) {
        return -1; /* opendir has already said why */
    }

    struct dirent **list = 0;
    size_t used = 0, cap = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != 0) {
        if (filter && !filter(ent)) {
            continue;
        }
        if (used == cap) {
            size_t next = cap ? cap * 2 : 32;
            struct dirent **grown = realloc(list, next * sizeof(*list));
            if (!grown) {
                goto nomem;
            }
            list = grown;
            cap = next;
        }
        /* A copy, because readdir's pointer belongs to the DIR and is
         * overwritten on the next call - which is the contract this
         * file's own comment states thirty lines up. */
        struct dirent *copy = malloc(sizeof(*copy));
        if (!copy) {
            goto nomem;
        }
        *copy = *ent;
        list[used++] = copy;
    }

    closedir(d);

    if (compar && used > 1) {
        scandir_cmp = compar;
        qsort(list, used, sizeof(*list), scandir_trampoline);
        scandir_cmp = 0;
    }

    *namelist = list;
    return (int)used;

nomem:
    for (size_t i = 0; i < used; i++) {
        free(list[i]);
    }
    free(list);
    closedir(d);
    errno = ENOMEM;
    return -1;
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcoll((*a)->d_name, (*b)->d_name);
}
