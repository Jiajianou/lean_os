/* user_space/bin/cat.c - M13 coreutil.
 *
 * M60: two changes, both of which are things `cat` has meant everywhere
 * for fifty years and neither of which this OS could express before.
 *
 * It takes *files*, plural, because SYS_spawn carries a real argument
 * vector now - `cat a b` was not a command this system could say.
 *
 * And with no arguments at all it reads standard input, which is what
 * makes it the right-hand side of a pipe. That needs one thing beyond a
 * loop: a read on a pipe has to be able to *end*, and it only can now
 * that M59 refcounts pipe ends - before it, "the last writer went away"
 * was not a knowable fact and `ls | cat` would have hung forever.
 *
 * Streamed through descriptors rather than read whole (M59), so what it
 * can print is no longer bounded by one buffer.
 */
#include "str.h"
#include "syscall_wrappers.h"

#define BUF_SIZE 4096

static char buf[BUF_SIZE];

/* Copies everything readable on `fd` to standard output. Returns 0, or 1
 * if a write failed part way - which for a pipe means the reader on the
 * other end is gone, and continuing would be shouting at nobody. */
static int drain(int fd) {
    for (;;) {
        long n = sys_read(fd, buf, BUF_SIZE);
        if (n <= 0) {
            return 0; /* end of file, or a read that cannot make progress */
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
