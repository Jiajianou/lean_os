/* user_space/init/init.c - PID 1 (M13). Spawns the compositor and the
 * desktop shell on top of it, waiting on the desktop shell; if it ever
 * exits, the compositor is killed and both are respawned fresh rather
 * than letting the machine sit at nothing - the same "restart the login
 * session" behavior a real init's getty loop has, kept to the minimum
 * that's actually meaningful here (no signals, no service table, no
 * runlevels - just "the desktop should always be there"). The text
 * shell (user_space/shell) is still on disk and still spawnable - e.g.
 * by clicking its launcher slot in desktop_shell's panel - it's just no
 * longer what init itself starts.
 */
#include "signal.h" /* system_api/include/signal.h - SIGKILL */
#include "syscall_wrappers.h"

int main(const char *arg) {
    (void)arg;
    for (;;) {
        long comp_pid = sys_spawn("compositor", "");
        if (comp_pid < 0) {
            /* Nothing to do if the compositor binary itself is missing -
             * there's no console to report to beyond what SYS_write
             * already reaches. */
            const char msg[] = "init: could not spawn compositor\n";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_exit(1);
        }

        long shell_pid = sys_spawn("desktop_shell", "");
        if (shell_pid < 0) {
            const char msg[] = "init: could not spawn desktop_shell\n";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_kill(comp_pid, SIGKILL);
            sys_wait(comp_pid);
            sys_exit(1);
        }

        sys_wait(shell_pid);

        /* The compositor never exits on its own (see compositor.c) - if
         * desktop_shell went away, tear it down too so the next loop
         * iteration starts both fresh instead of leaking a compositor
         * with no panel left to query/action it. */
        sys_kill(comp_pid, SIGKILL);
        sys_wait(comp_pid);
    }
    return 0;
}
