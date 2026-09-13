#include <stdio.h>
#include <string.h>

#include "capabilities.h"
#include "syscall_wrappers.h"

int main(int argc, char **argv) {
    int show_all = (argc > 1 && strcmp(argv[1], "-a") == 0);
    uint32_t mine = (uint32_t)sys_getcaps();

    for (int i = 0; i < CAP_NAME_COUNT; i++) {
        int held = (mine & CAP_NAMES[i].bit) != 0;
        if (held || show_all) {
            printf("%s %s\n", held ? "yes" : " no", CAP_NAMES[i].name);
        }
    }
    if (!mine) {
        printf("(none)\n");
    }
    return 0;
}
