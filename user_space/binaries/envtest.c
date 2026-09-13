#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "paths.h"
#include "syscall_wrappers.h"

int main(void) {
    const char *name = getenv("M75_OUT");
    const char *body = getenv("M75_BODY");
    if (!name || !name[0] || !body || !body[0]) {
        return 2;
    }
    if (getenv("M75_ABSENT")) {
        return 4;
    }

    char cwd[PATH_MAX_LENGTH];
    if (!getcwd(cwd, sizeof(cwd)) || cwd[0] != '/') {
        return 3;
    }

    long fd = sys_open(name, OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (fd < 0) {
        return 5;
    }
    char line[PATH_MAX_LENGTH + 64];
    int n = 0;
    for (const char *s = cwd; *s && n < (int)sizeof(line) - 2; s++) {
        line[n++] = *s;
    }
    line[n++] = ' ';
    for (const char *s = body; *s && n < (int)sizeof(line) - 1; s++) {
        line[n++] = *s;
    }
    long wrote = sys_write((int)fd, line, (size_t)n);
    sys_close((int)fd);
    return wrote == n ? 0 : 5;
}
