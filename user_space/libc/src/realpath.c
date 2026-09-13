#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REALPATH_MAX_LINKS 40

char *realpath(const char *path, char *resolved) {
    if (!path) {
        errno = EINVAL;
        return (char *)0;
    }
    if (!path[0]) {
        errno = ENOENT;
        return (char *)0;
    }

    char left[PATH_MAX];
    char out[PATH_MAX];
    size_t outlen;
    int links = 0;

    if (path[0] == '/') {
        if (strlen(path) >= sizeof(left)) {
            errno = ENAMETOOLONG;
            return (char *)0;
        }
        strcpy(left, path + 1);
        out[0] = '/';
        out[1] = '\0';
        outlen = 1;
    } else {
        if (!getcwd(out, sizeof(out))) {
            return (char *)0;
        }
        outlen = strlen(out);
        if (strlen(path) >= sizeof(left)) {
            errno = ENAMETOOLONG;
            return (char *)0;
        }
        strcpy(left, path);
    }

    while (left[0]) {
        char *slash = strchr(left, '/');
        size_t clen = slash ? (size_t)(slash - left) : strlen(left);
        char comp[PATH_MAX];
        memcpy(comp, left, clen);
        comp[clen] = '\0';
        if (slash) {
            memmove(left, slash + 1, strlen(slash + 1) + 1);
        } else {
            left[0] = '\0';
        }

        if (clen == 0 || strcmp(comp, ".") == 0) {
            continue;
        }
        if (strcmp(comp, "..") == 0) {
            while (outlen > 1 && out[outlen - 1] != '/') {
                outlen--;
            }
            if (outlen > 1) {
                outlen--;
            }
            out[outlen] = '\0';
            continue;
        }

        size_t need = outlen + (outlen > 1 ? 1 : 0) + clen;
        if (need >= sizeof(out)) {
            errno = ENAMETOOLONG;
            return (char *)0;
        }
        if (outlen > 1) {
            out[outlen++] = '/';
        }
        memcpy(out + outlen, comp, clen + 1);
        outlen += clen;

        struct stat st;
        if (lstat(out, &st) != 0) {
            return (char *)0;
        }
        if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            long n = readlink(out, target, sizeof(target) - 1);
            if (n < 0) {
                errno = EIO;
                return (char *)0;
            }
            target[n] = '\0';
            if (++links > REALPATH_MAX_LINKS) {
                errno = ELOOP;
                return (char *)0;
            }
            size_t tlen = strlen(target);
            size_t llen = strlen(left);
            if (tlen + 1 + llen + 1 > sizeof(left)) {
                errno = ENAMETOOLONG;
                return (char *)0;
            }
            char joined[PATH_MAX];
            memcpy(joined, target, tlen);
            size_t j = tlen;
            if (llen) {
                joined[j++] = '/';
                memcpy(joined + j, left, llen);
                j += llen;
            }
            joined[j] = '\0';
            strcpy(left, joined);
            outlen -= clen;
            if (outlen > 1) {
                outlen--;
            }
            out[outlen] = '\0';
            if (target[0] == '/') {
                out[0] = '/';
                out[1] = '\0';
                outlen = 1;
                memmove(left, left + 1, strlen(left + 1) + 1);
            }
            continue;
        }
        if (left[0] && !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return (char *)0;
        }
    }

    if (!resolved) {
        resolved = (char *)malloc(outlen + 1);
        if (!resolved) {
            errno = ENOMEM;
            return (char *)0;
        }
    }
    memcpy(resolved, out, outlen + 1);
    return resolved;
}
