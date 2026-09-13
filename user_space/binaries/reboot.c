#include "power_mode.h"
#include "syscall_wrappers.h"

int main(void) {
    const char message[] = "Restarting...\n";
    sys_write(1, message, sizeof(message) - 1);
    sys_shutdown(POWER_REBOOT);
    return 1;
}
