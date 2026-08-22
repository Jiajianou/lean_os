/* user_space/init/init.c - PID 1 (M13). Spawns the shell and waits for
 * it; if the shell ever exits (e.g. via its `exit` command), spawns a
 * fresh one rather than letting the machine sit at nothing - the same
 * "restart the login shell" behavior a real init's getty loop has, kept
 * to the minimum that's actually meaningful here (no signals, no service
 * table, no runlevels - just "the shell should always be there").
 */
#include "syscall_wrappers.h"

int main(const char *arg) {
    (void)arg;
    for (;;) {
        long pid = sys_spawn("shell", "");
        if (pid < 0) {
            /* Nothing to do if the shell binary itself is missing -
             * there's no console to report to beyond what SYS_write
             * already reaches. */
            const char msg[] = "init: could not spawn shell\n";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_exit(1);
        }
        sys_wait(pid);
    }
    return 0;
}
