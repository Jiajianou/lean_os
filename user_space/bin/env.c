/* user_space/bin/env.c - M75
 *
 * Prints the environment, one NAME=value per line. The same reason
 * `netconf` and `caps` exist: the thing that makes a subsystem checkable
 * is a program that prints what it did. An environment that is inherited
 * across a spawn is a claim, and this is how you see it.
 *
 * With arguments, it runs one: `env NAME=value program args...` sets what
 * it is given and then spawns, which is the other half of what /bin/env
 * is for everywhere - and here it is also the shortest way to prove that
 * a *child* sees what its parent set, without a shell in the middle.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "paths.h"
#include "syscall_wrappers.h"

static void out(const char *s) {
    sys_write(1, s, strlen(s));
}

int main(int argc, char **argv) {
    int i = 1;
    /* Leading NAME=value assignments, exactly as /bin/env takes them. */
    for (; i < argc && strchr(argv[i], '=') && strchr(argv[i], '=') != argv[i]; i++) {
        char name[64];
        const char *eq = strchr(argv[i], '=');
        int n = (int)(eq - argv[i]);
        if (n > (int)sizeof(name) - 1) {
            n = (int)sizeof(name) - 1;
        }
        memcpy(name, argv[i], (size_t)n);
        name[n] = '\0';
        setenv(name, eq + 1, 1);
    }

    if (i >= argc) {
        for (int e = 0; environ && environ[e]; e++) {
            out(environ[e]);
            out("\n");
        }
        return 0;
    }

    /* A program to run. A name with no '/' is looked up in $PATH, which
     * is exactly what this program has just been asked to be able to
     * change. */
    char path[PATH_MAX_LEN];
    if (strchr(argv[i], '/')) {
        int n = 0;
        for (; argv[i][n] && n < PATH_MAX_LEN - 1; n++) {
            path[n] = argv[i][n];
        }
        path[n] = '\0';
    } else {
        const char *dir = getenv("PATH");
        if (!dir || !dir[0]) {
            dir = PATH_BIN;
        }
        int n = 0;
        for (; dir[n] && n < PATH_MAX_LEN - 2; n++) {
            path[n] = dir[n];
        }
        if (n == 0 || path[n - 1] != '/') {
            path[n++] = '/';
        }
        for (int k = 0; argv[i][k] && n < PATH_MAX_LEN - 1; k++) {
            path[n++] = argv[i][k];
        }
        path[n] = '\0';
    }

    /* argv[1..] for the child - the kernel supplies argv[0] itself. */
    long pid = sys_spawnv(path, (const char *const *)&argv[i + 1]);
    if (pid < 0) {
        out("env: cannot run ");
        out(path);
        out("\n");
        return 127;
    }
    long status = sys_wait(pid);
    return status < 0 ? 1 : (int)status;
}
