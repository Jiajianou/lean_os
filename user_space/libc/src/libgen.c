/* user_space/libc/src/libgen.c - M89
 *
 * basename and dirname, both of which may write to the string they are
 * given - see <libgen.h>. The edge cases below are the ones POSIX
 * enumerates and they are the whole difficulty of these two functions:
 * "/" , "" , "/usr/" and "usr" all have answers that are not what a
 * first implementation produces.
 */
#include <libgen.h>

static char dot[] = ".";
static char slash[] = "/";

char *basename(char *path) {
    if (!path || !*path) {
        return dot; /* POSIX: an empty path is "." */
    }
    /* Trailing slashes are not part of the name: basename("/usr/") is
     * "usr", not "". */
    char *end = path;
    while (*end) {
        end++;
    }
    while (end > path && end[-1] == '/') {
        end--;
    }
    if (end == path) {
        return slash; /* the path was all slashes, so the answer is "/" */
    }
    *end = '\0';
    char *start = end;
    while (start > path && start[-1] != '/') {
        start--;
    }
    return start;
}

char *dirname(char *path) {
    if (!path || !*path) {
        return dot;
    }
    char *end = path;
    while (*end) {
        end++;
    }
    while (end > path && end[-1] == '/') {
        end--;
    }
    /* The path was nothing but slashes, and its directory is the root.
     * This test has to come BEFORE the component scan below, which was
     * the bug the POSIX table caught: with it missing, "/" stripped down
     * to an empty range, the scan found no slash in it, and the function
     * reported "." - the answer for a bare relative name, which is the
     * one thing "/" certainly is not. */
    if (end == path) {
        return slash;
    }
    /* Back over the last component. */
    while (end > path && end[-1] != '/') {
        end--;
    }
    if (end == path) {
        return dot; /* no slash at all: dirname("usr") is "." */
    }
    /* And over the slashes that separated it, so dirname("/usr//lib") is
     * "/usr" rather than "/usr/". */
    while (end > path && end[-1] == '/') {
        end--;
    }
    if (end == path) {
        return slash; /* dirname("/usr") is "/" */
    }
    *end = '\0';
    return path;
}
