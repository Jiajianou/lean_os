#include <sys/reboot.h>

#include <errno.h>

#include "power_mode.h"
#include "syscall_wrappers.h"

int reboot(int cmd) {
    switch ((unsigned int)cmd) {
    case RB_AUTOBOOT:
        sys_shutdown(POWER_REBOOT);
        break;
    case RB_POWER_OFF:
        sys_shutdown(POWER_OFF);
        break;
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
    errno = EINVAL;
    return -1;
}
