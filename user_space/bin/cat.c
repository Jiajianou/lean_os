/* user_space/bin/cat.c - M13 coreutil: prints a file's contents via
 * SYS_readfile (whole-file read by name - no open/lseek/fd table yet). */
#include "str.h"
#include "syscall_wrappers.h"

#define BUF_SIZE 8192

static char buf[BUF_SIZE];

int main(const char *arg) {
    if (strlen(arg) == 0) {
        const char msg[] = "usage: cat <file>\n";
        sys_write(1, msg, strlen(msg));
        return 1;
    }

    long n = sys_readfile(arg, buf, BUF_SIZE);
    if (n < 0) {
        const char msg[] = "cat: no such file\n";
        sys_write(1, msg, strlen(msg));
        return 1;
    }

    sys_write(1, buf, (size_t)n);
    return 0;
}
