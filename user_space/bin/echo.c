/* user_space/bin/echo.c - M13 coreutil. `arg` is the whole rest of the
 * command line the shell handed to sys_spawn (lean_os's one-string
 * "argv" - see kernel/proc/proc.c), not a real argv array. */
#include "str.h"
#include "syscall_wrappers.h"

int main(int argc, char **argv) {
    /* M60: every argument, space separated - which is what echo has meant
     * everywhere for fifty years and what this could not do while a
     * process was handed one string. argv[0] is this program's own path
     * and is deliberately not echoed. */
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            sys_write(1, " ", 1);
        }
        sys_write(1, argv[i], strlen(argv[i]));
    }
    sys_write(1, "\n", 1);
    return 0;
}
