/* user_space/bin/ls.c - M13 coreutil: lists every file on leanfs, which
 * is flat (no subdirectories), so there's no path argument to take. */
#include "syscall_wrappers.h"

#define BUF_SIZE 2048

static char buf[BUF_SIZE];

int main(const char *arg) {
    (void)arg;
    long n = sys_listfiles(buf, BUF_SIZE);
    if (n < 0) {
        const char msg[] = "ls: error listing files\n";
        sys_write(1, msg, sizeof(msg) - 1);
        return 1;
    }
    sys_write(1, buf, (size_t)n);
    return 0;
}
