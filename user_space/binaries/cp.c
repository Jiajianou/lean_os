#include "string_utilities.h"
#include "syscall_wrappers.h"

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
            say("cp: ran out of space\n");
            rc = 1;
            break;
        }
    }
    sys_close((int)src);
    sys_close((int)dst);
    return rc;
}
