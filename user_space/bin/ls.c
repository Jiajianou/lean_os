/* user_space/bin/ls.c - M13 coreutil.
 *
 * M53: takes a path. leanfs was flat until then, so there was nothing to
 * take one *for* - "every file there is" was the only listing available.
 * With no argument this lists "/", not a working directory: this project
 * has no such concept, so there is nothing else "here" could honestly
 * mean.
 *
 * A name that comes back with a trailing '/' is a directory - SYS_listdir
 * marks them so a caller needn't ask a second time - and that marker is
 * printed as-is, which is exactly what `ls -F` does and is the cheapest
 * useful thing to show.
 */
#include "paths.h" /* system_api/include/paths.h - M53's layout */
#include "syscall_wrappers.h"

#define BUF_SIZE 2048

static char buf[BUF_SIZE];

static void put(const char *s) {
    long n = 0;
    while (s[n]) {
        n++;
    }
    sys_write(1, s, (size_t)n);
}

int main(int argc, char **argv) {
    /* M60: argv[0] is this program's own path; argv[1] is the first thing
     * the caller had to say. `arg` keeps the name the body already uses,
     * and is the empty string when there was nothing - which is exactly
     * what the single-string mechanism this replaced handed over. */
    const char *arg = argc > 1 ? argv[1] : "";
    const char *path = "/";
    if (arg && arg[0]) {
        path = arg;
    }
    long n = sys_listdir(path, buf, BUF_SIZE);
    if (n < 0) {
        put("ls: ");
        put(path);
        put(": not a directory\n");
        return 1;
    }
    sys_write(1, buf, (size_t)n);
    return 0;
}
