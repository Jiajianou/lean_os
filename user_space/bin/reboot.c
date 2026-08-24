/* user_space/bin/reboot.c
 *
 * M47: the restart half of shutdown.c - see that file's header for why
 * both are programs on disk rather than shell builtins. Kept as its own
 * two-line program rather than `shutdown --reboot`, because this project
 * has no argument parsing beyond one opaque string (kernel/proc/proc.c's
 * whole "argv") and a second *name* is what makes it typeable.
 */
#include "power_mode.h"
#include "syscall_wrappers.h"

int main(const char *arg) {
    (void)arg;
    const char msg[] = "Restarting...\n";
    sys_write(1, msg, sizeof(msg) - 1);
    sys_shutdown(POWER_REBOOT);
    return 1;
}
