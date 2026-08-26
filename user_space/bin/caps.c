/* user_space/bin/caps.c
 *
 * M65: what this process is allowed to do.
 *
 * The counterpart to `netconf` and here for the same reason: a rule
 * nobody can see is a rule nobody can check. Run it from the shell and
 * you get the shell's descendants' set; run it under something more
 * restricted and you get that instead.
 *
 * `caps -a` lists every capability that exists, held or not, because
 * "what could I have been given" is a different question from "what do
 * I have" and the second one is much less useful on its own.
 */
#include <stdio.h>
#include <string.h>

#include "caps.h"
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
