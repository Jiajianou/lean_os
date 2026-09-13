#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "syscall_wrappers.h"

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        if (sys_setsid() < 0) {
            return 2;
        }
    }
    if (sys_setpgid(0, 0) != 0) {
        return 2;
    }

    for (int i = 0; i < 20000; i++) {
        sys_yield();
    }
    printf("jobtest: finished undisturbed\n");
    return 0;
}
