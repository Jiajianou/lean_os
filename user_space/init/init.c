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
#include <stdlib.h> /* setenv - M75 */
#include <unistd.h> /* chdir - M75 */

#include "paths.h" /* system_api/include/paths.h - M53: /bin is where programs live now */
#include "signal.h" /* system_api/include/signal.h - SIGKILL */
#include "syscall_wrappers.h"

int main(int argc, char **argv) {
    /* M60: argv[0] is this program's own path; argv[1] is the first thing
     * the caller had to say. `arg` keeps the name the body already uses,
     * and is the empty string when there was nothing - which is exactly
     * what the single-string mechanism this replaced handed over. */
    const char *arg = argc > 1 ? argv[1] : "";
    (void)arg;

    /* ---- M75: the environment every process on this machine descends
     * from, and the directory it starts in.
     *
     * PID 1 is the right place for both. The kernel deliberately does not
     * invent an environment - a kernel that knew what HOME meant would be
     * a kernel with an opinion about a user space it cannot see - so
     * something in user space has to be first, and "first" is exactly
     * what init is. Everything spawned from here inherits these, which is
     * what makes `$HOME` mean something in a shell nobody passed it to.
     *
     * /home rather than /: a desktop's programs are opened *somewhere*,
     * and the somewhere a person's files are is the only defensible
     * default. Every path in this OS was absolute before this milestone,
     * so nothing existing changes behaviour - the directory only starts
     * mattering the moment something says a relative name. */
    setenv("HOME", PATH_HOME, 1);
    setenv("PATH", PATH_BIN, 1);
    setenv("TMPDIR", PATH_TMP, 1);
    setenv("SHELL", PATH_BIN_DIR "sh", 1);
    /* ---- M74: the session gate ---------------------------------------
     *
     * Set here and nowhere else. The compositor saves and restores a
     * session only when it finds this in its environment, and the boot
     * self-tests start compositors constantly - each one opening windows
     * and killing them. A self-test compositor that saved a session would
     * leave a file the real one then restored, relaunching test fixtures
     * onto a person's desktop.
     *
     * An environment variable rather than an argument, which is the one
     * thing that changed since this milestone's first attempt: gating on
     * argv meant giving the compositor's `main` two parameters it had
     * never had, and that change was one of the two suspects left when
     * that attempt was reverted. M75 made an inherited environment exist,
     * so the gate is now a thing PID 1 says once. */
    setenv("LEANOS_SESSION", "1", 1);
    chdir(PATH_HOME);

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
