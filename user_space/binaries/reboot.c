#include "power_mode.h"
#include "syscall_wrappers.h"

int main(void) {
    const char msg[] = "Restarting...\n";
    sys_write(1, msg, sizeof(msg) - 1);
    sys_shutdown(POWER_REBOOT);
    return 1;
}
