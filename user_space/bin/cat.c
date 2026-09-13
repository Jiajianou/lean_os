#include "str.h"
#include "syscall_wrappers.h"

#define BUF_SIZE 4096

static char buf[BUF_SIZE];

static int drain(int fd) {
    for (;;) {
        long n = sys_read(fd, buf, BUF_SIZE);
        if (n <= 0) {
            return 0;
        }
        if (sys_write(1, buf, (size_t)n) != n) {
            return 1;
        }
    }
}

int main(int argc, char **argv) {
    if (argc <= 1) {
        return drain(0);
    }

    int failures = 0;
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], OPEN_READ);
        if (fd < 0) {
            const char msg[] = "cat: no such file: ";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_write(1, argv[i], strlen(argv[i]));
            sys_write(1, "\n", 1);
            failures = 1;
            continue;
        }
        failures |= drain((int)fd);
        sys_close((int)fd);
    }
    return failures;
}
