#include <stdlib.h>
#include <unistd.h>

#include "paths.h"
#include "signal.h"
#include "syscall_wrappers.h"

int main(int argc, char **argv) {
    const char *arg = argc > 1 ? argv[1] : "";
    (void)arg;

    setenv("HOME", PATH_HOME, 1);
    setenv("PATH", PATH_BIN, 1);
    setenv("TMPDIR", PATH_TMP, 1);
    setenv("SHELL", PATH_BIN_DIR "sh", 1);
    setenv("LEANOS_SESSION", "1", 1);
    chdir(PATH_HOME);

    long os_pid = sys_spawn(PATH_BIN_DIR "os", "preinstall");
    if (os_pid >= 0) {
        sys_wait(os_pid);
    }

    int first_compositor = 1;

    for (;;) {
        if (!first_compositor) {
            setenv("LEANOS_SESSION", "reconnect", 1);
        }
        first_compositor = 0;
        long comp_pid = sys_spawn(PATH_BIN_DIR "compositor", "");
        if (comp_pid < 0) {
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
            long until = sys_uptime_ms() + 250;
            while (sys_uptime_ms() < until) {
                sys_yield();
            }
        }

        sys_kill(shell_pid, SIGKILL);
        sys_wait(shell_pid);
        sys_kill(icons_pid, SIGKILL);
        sys_wait(icons_pid);
        sys_kill(comp_pid, SIGKILL);
        sys_wait(comp_pid);
    }
    return 0;
}
