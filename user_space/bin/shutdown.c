/* user_space/bin/shutdown.c
 *
 * M47: `shutdown` at a prompt. Half the time you are already in the
 * terminal, and it gives the self-tests and the input harness a
 * non-graphical way into the same SYS_shutdown the launcher's Power
 * control reaches - the compositor and this program call the identical
 * syscall, so neither can drift from the other.
 *
 * A program on disk rather than a shell builtin, like every other command
 * this OS ships: that is what makes it reachable from gui_terminal.c and
 * from M43's launcher as well as from the text shell.
 */
#include "power_mode.h"
#include "syscall_wrappers.h"

int main(const char *arg) {
    (void)arg;
    const char msg[] = "Shutting down...\n";
    sys_write(1, msg, sizeof(msg) - 1);
    sys_shutdown(POWER_OFF);
    /* Only reachable if the kernel refused the mode, which it cannot for
     * this one - so reaching here at all is a bug worth a nonzero exit. */
    return 1;
}
