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

        sys_wait(shell_pid);

        /* The compositor and desktop_icons never exit on their own (see
         * compositor.c / desktop_icons.c) - if desktop_shell went away,
         * tear them both down so the next loop iteration starts a whole
         * fresh session instead of leaking a compositor with no taskbar
         * left to query/action it. */
        sys_kill(icons_pid, SIGKILL);
        sys_wait(icons_pid);
        sys_kill(comp_pid, SIGKILL);
        sys_wait(comp_pid);
    }
    return 0;
}
