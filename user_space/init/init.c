/* user_space/init/init.c - PID 1 (M13). Spawns the compositor, the
 * desktop background/icons and the desktop shell (the taskbar), waiting
 * on the desktop shell; if it ever exits, the others are killed and the
 * whole session is respawned fresh rather than letting the machine sit at
 * nothing - the same "restart the login session" behavior a real init's
 * getty loop has, kept to the minimum that's actually meaningful here (no
 * signals, no service table, no runlevels - just "the desktop should
 * always be there"). The text shell (user_space/shell) is still on disk
 * and still spawnable - e.g. from a desktop icon - it's just no longer
 * what init itself starts.
 *
 * M41 added a fourth client here, a top-docked menu bar; M42 deleted it
 * again in favor of a single bottom taskbar, so this is back to three.
 */
#include "paths.h" /* system_api/include/paths.h - M53: /bin is where programs live now */
#include "signal.h" /* system_api/include/signal.h - SIGKILL */
#include "syscall_wrappers.h"

int main(const char *arg) {
    (void)arg;
    for (;;) {
        long comp_pid = sys_spawn(PATH_BIN_DIR "compositor", "");
        if (comp_pid < 0) {
            /* Nothing to do if the compositor binary itself is missing -
             * there's no console to report to beyond what SYS_write
             * already reaches. */
            const char msg[] = "init: could not spawn compositor\n";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_exit(1);
        }

        long icons_pid = sys_spawn(PATH_BIN_DIR "desktop_icons", "");
        if (icons_pid < 0) {
            const char msg[] = "init: could not spawn desktop_icons\n";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_kill(comp_pid, SIGKILL);
            sys_wait(comp_pid);
            sys_exit(1);
        }

        long shell_pid = sys_spawn(PATH_BIN_DIR "desktop_shell", "");
        if (shell_pid < 0) {
            const char msg[] = "init: could not spawn desktop_shell\n";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_kill(icons_pid, SIGKILL);
            sys_wait(icons_pid);
            sys_kill(comp_pid, SIGKILL);
            sys_wait(comp_pid);
            sys_exit(1);
        }

        /* M55: all three, not just the taskbar. The old arrangement was
         * not a design - it was `sys_wait` taking one pid, and it meant a
         * compositor or a desktop_icons that died on its own was not
         * noticed until desktop_shell happened to die too. Which, for the
         * compositor, is "never": nothing else in this session exits when
         * the screen goes away.
         *
         * Polled rather than waited on because there is no wait-for-any
         * primitive here that distinguishes *which* child went. SYS_wait
         * with -1 reaps an arbitrary one and this loop needs to know
         * which pid to stop watching; SYS_task_alive answers exactly the
         * question being asked and costs one syscall per child per
         * quarter second. */
        long watched[] = {comp_pid, icons_pid, shell_pid};
        for (;;) {
            int lost = 0;
            for (int i = 0; i < 3; i++) {
                if (sys_task_alive(watched[i]) <= 0) {
                    lost = 1;
                }
            }
            if (lost) {
                break;
            }
            /* A quarter second: fast enough that a dead session is
             * noticed before anyone reaches for the reset button, slow
             * enough that PID 1 is not a busy loop. sys_yield alone
             * would be - there is no sleep in this project, so this is
             * a yield inside a wall-clock bound. */
            long until = sys_uptime_ms() + 250;
            while (sys_uptime_ms() < until) {
                sys_yield();
            }
        }

        /* Whichever one went, the other two go with it - a compositor
         * with no taskbar left to query it, or a taskbar with no screen
         * to draw on, is not a session anybody can use. Killing an
         * already-dead pid is a harmless -1, and the waits are what give
         * their task slots back (M54's children.h reasoning, applied to
         * the one process that has always been these three's parent). */
        sys_kill(shell_pid, SIGKILL);
        sys_wait(shell_pid);
        sys_kill(icons_pid, SIGKILL);
        sys_wait(icons_pid);
        sys_kill(comp_pid, SIGKILL);
        sys_wait(comp_pid);
    }
    return 0;
}
