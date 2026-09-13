#include "power_mode.h"
#include "syscall_wrappers.h"

int main(void) {
    const char msg[] = "Shutting down...\n";
    sys_write(1, msg, sizeof(msg) - 1);
    sys_shutdown(POWER_OFF);
    return 1;
}
