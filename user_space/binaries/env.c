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

    char path[PATH_MAX_LENGTH];
    if (strchr(argv[i], '/')) {
        int n = 0;
        for (; argv[i][n] && n < PATH_MAX_LENGTH - 1; n++) {
            path[n] = argv[i][n];
        }
        path[n] = '\0';
    } else {
        const char *directory = getenv("PATH");
        if (!directory || !directory[0]) {
            directory = PATH_BIN;
        }
        int n = 0;
        for (; directory[n] && n < PATH_MAX_LENGTH - 2; n++) {
            path[n] = directory[n];
        }
        if (n == 0 || path[n - 1] != '/') {
            path[n++] = '/';
        }
        for (int k = 0; argv[i][k] && n < PATH_MAX_LENGTH - 1; k++) {
            path[n++] = argv[i][k];
        }
        path[n] = '\0';
    }

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
