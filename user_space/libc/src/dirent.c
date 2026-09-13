#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "paths.h"
#include "syscall.h"
#include "syscall_wrappers.h"

#define DIRECTORY_BUFFER 4096

_Static_assert(DIRECTORY_BUFFER >= OS_DIRENT_MAX, "a fetch buffer must hold the longest single record");

struct DIR {
    unsigned int cookie;
    int fd;
    long length;
    long position;
    int at_end;
    char path[PATH_MAX_LENGTH];
    struct dirent entry;
    char buffer[DIRECTORY_BUFFER] __attribute__((aligned(8)));
};

DIR *opendir(const char *path) {
    if (!path) {
        return 0;
    }
    size_t plen = strlen(path);
    if (plen >= PATH_MAX_LENGTH) {
        return 0;
    }

    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) {
        return 0;
    }
    memcpy(d->path, path, plen + 1);
    d->fd = -1;
    d->cookie = 0;
    d->length = 0;
    d->position = 0;
    d->at_end = 0;

    long n = sys_getdents(d->path, &d->cookie, d->buffer, sizeof(d->buffer));
    if (n < 0) {
        free(d);
        return 0;
    }
    d->length = n;
    d->at_end = (n == 0);
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) {
        return 0;
    }
    for (;;) {
        if (d->position < d->length) {
            const os_dirent_t *r = (const os_dirent_t *)(const void *)(d->buffer + d->position);
            if (r->reclen < sizeof(os_dirent_t) || d->position + r->reclen > d->length) {
                return 0;
            }
            size_t n = r->name_length;
            if (n > NAME_MAX) {
                n = NAME_MAX;
            }
            memcpy(d->entry.d_name, r->name, n);
            d->entry.d_name[n] = '\0';
            d->entry.d_type = r->type;
            d->entry.d_ino = r->ino;
            d->position += r->reclen;
            return &d->entry;
        }
        if (d->at_end) {
            return 0;
        }
        long n = sys_getdents(d->path, &d->cookie, d->buffer, sizeof(d->buffer));
        if (n <= 0) {
            d->at_end = 1;
            return 0;
        }
        d->length = n;
        d->position = 0;
    }
}

void rewinddir(DIR *d) {
    if (!d) {
        return;
    }
    d->cookie = 0;
    d->length = 0;
    d->position = 0;
    d->at_end = 0;
    long n = sys_getdents(d->path, &d->cookie, d->buffer, sizeof(d->buffer));
    if (n > 0) {
        d->length = n;
    } else {
        d->at_end = 1;
    }
}

int closedir(DIR *d) {
    if (!d) {
        return -1;
    }
    if (d->fd >= 0) {
        close(d->fd);
    }
    free(d);
    return 0;
}

DIR *fdopendir(int fd) {
    char path[PATH_MAX_LENGTH];
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
        errno = ENOTSUP;
        return -1;
    }
    return d->fd;
}

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
        return -1;
    }

    struct dirent **list = 0;
    size_t used = 0, cap = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != 0) {
        if (filter && !filter(entry)) {
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
        struct dirent *copy = malloc(sizeof(*copy));
        if (!copy) {
            goto nomem;
        }
        *copy = *entry;
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
