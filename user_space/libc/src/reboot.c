/* user_space/libc/src/reboot.c - M89. See <sys/reboot.h>. */
#include <sys/reboot.h>

#include <errno.h>

#include "power_mode.h" /* system_api/include/power_mode.h */
#include "syscall_wrappers.h"

int reboot(int cmd) {
    switch ((unsigned int)cmd) {
    case RB_AUTOBOOT:
        sys_shutdown(POWER_REBOOT);
        break;
    case RB_POWER_OFF:
        sys_shutdown(POWER_OFF);
        break;
    /* Ctrl-Alt-Del handling: there is no such key handler in this
     * kernel's keyboard driver, so enabling or disabling it is a request
     * about a thing that does not exist. A success would be the honest
     * answer if the default were "disabled" - it is, in the sense that
     * nothing happens - so this succeeds for DISABLE and refuses ENABLE,
     * which is the pair of answers that are both true. */
    case RB_DISABLE_CAD:
        return 0;
    case RB_ENABLE_CAD:
    case RB_HALT_SYSTEM:
    case RB_SW_SUSPEND:
    case RB_KEXEC:
    default:
        errno = EINVAL;
        return -1;
    }
    /* Only reached if the shutdown itself was refused - see SYS_shutdown,
     * which does not return on success. */
    errno = EINVAL;
    return -1;
}
