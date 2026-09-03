/* user_space/libc/src/realpath.c - M98
 *
 * The canonical absolute pathname: relative made absolute against the
 * real working directory, "." and ".." resolved by walking rather than
 * by string surgery, and every symbolic link followed as it is met -
 * which is the only order that gets "a/../b" right when `a` is a link.
 *
 * Why now: libiberty's lrealpath() has canonicalization strategies for
 * five kinds of system and, on a system with none of them, falls off
 * the end of a non-void function. The gcc driver calls it twice to ask
 * "is the input file the output file", compared the two leftovers of
 * whatever was in a register, found them equal, and refused to compile
 * anything with -o. A libc that does not provide realpath is not
 * missing a convenience; it is feeding undefined behaviour to every
 * program whose configure found nothing better.
 *
 * POSIX semantics, including the strict one: every component must
 * exist, or NULL comes back with errno saying why - which is exactly
 * what makes lrealpath fall back to the plain name for a not-yet-
 * created -o target.
 *
 * Compiled for the host and graded against the host's own realpath by
 * tools/realpath-test.sh, over a fixture tree of real directories and
 * real symlinks - the same argument as the other differential tests:
 * this function's whole job is to agree with everyone else's.
 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Splices links no more than this many times before calling the path
 * circular. Linux uses 40; agreeing with it costs nothing. */
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

    /* `left` is what remains to be resolved, `out` is the resolved
     * prefix - both bounded by PATH_MAX, like the result. */
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
            return (char *)0; /* getcwd set errno */
        }
        outlen = strlen(out);
        if (strlen(path) >= sizeof(left)) {
            errno = ENAMETOOLONG;
            return (char *)0;
        }
        strcpy(left, path);
    }

    while (left[0]) {
        /* Peel the first component off `left`. */
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
            /* Walk `out` up one component - never above the root:
             * outlen starts at 1 for "/" and only sheds characters
             * while it is greater than that. */
            while (outlen > 1 && out[outlen - 1] != '/') {
                outlen--;
            }
            if (outlen > 1) {
                outlen--; /* drop the slash too */
            }
            out[outlen] = '\0';
            continue;
        }

        /* Append the component. */
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

        /* What did we just name? A link is spliced back onto `left`
         * and removed from `out`; anything else must simply exist -
         * and, when components remain, be a directory. */
        struct stat st;
        if (lstat(out, &st) != 0) {
            return (char *)0; /* lstat set errno (ENOENT) */
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
            /* left = target [+ "/" + left] */
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
            /* Remove the link's name from `out`. An absolute target
             * additionally resets `out` to the root. */
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
