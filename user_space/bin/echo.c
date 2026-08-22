/* user_space/bin/echo.c - M13 coreutil. `arg` is the whole rest of the
 * command line the shell handed to sys_spawn (lean_os's one-string
 * "argv" - see kernel/proc/proc.c), not a real argv array. */
#include "str.h"
#include "syscall_wrappers.h"

int main(const char *arg) {
    sys_write(1, arg, strlen(arg));
    sys_write(1, "\n", 1);
    return 0;
}
