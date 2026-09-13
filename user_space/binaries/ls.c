#include "paths.h"
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
