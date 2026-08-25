/* user_space/bin/cp.c
 *
 * M60, and it exists because of the milestone rather than alongside it:
 * `cp a b` is the example M59 and M60 both used for what this OS could
 * not express. A process was handed one string, so a program could be
 * told one thing - and the only honest way to write `cp` was not to.
 *
 * Two things had to land first. A real argument vector (this milestone),
 * so "a" and "b" arrive as two arguments rather than as one string
 * somebody has to re-split; and descriptors (M59), so a copy is a loop
 * over a chunk buffer rather than "read the whole file into memory and
 * hope it fits", which is what file_manager.c had been apologising for
 * since M56.
 */
#include "str.h"
#include "syscall_wrappers.h"

/* Eight sectors - a whole ATA transfer's worth, and small enough to be
 * ordinary static storage in a program this size. */
#define CHUNK 4096

static char chunk[CHUNK];

static void say(const char *s) {
    sys_write(1, s, strlen(s));
}

int main(int argc, char **argv) {
    if (argc != 3) {
        say("usage: cp <from> <to>\n");
        return 1;
    }

    long src = sys_open(argv[1], OPEN_READ);
    if (src < 0) {
        say("cp: cannot read ");
        say(argv[1]);
        say("\n");
        return 1;
    }
    long dst = sys_open(argv[2], OPEN_WRITE | OPEN_CREATE | OPEN_TRUNCATE);
    if (dst < 0) {
        say("cp: cannot write ");
        say(argv[2]);
        say("\n");
        sys_close((int)src);
        return 1;
    }

    int rc = 0;
    for (;;) {
        long n = sys_read((int)src, chunk, sizeof(chunk));
        if (n < 0) {
            say("cp: read failed\n");
            rc = 1;
            break;
        }
        if (n == 0) {
            break;
        }
        if (sys_write((int)dst, chunk, (size_t)n) != n) {
            /* A short write is a full disk. The destination is left as
             * far as it got rather than removed: deleting a file on
             * somebody's behalf because a copy of it failed is a worse
             * outcome than leaving a short one they can see. */
            say("cp: ran out of space\n");
            rc = 1;
            break;
        }
    }
    sys_close((int)src);
    sys_close((int)dst);
    return rc;
}
