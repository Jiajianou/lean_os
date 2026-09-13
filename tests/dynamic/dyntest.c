#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char banner[] = "dyntest";

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "share") == 0) {
        for (int i = 0; i < 60; i++) {
            usleep(50000);
        }
        return 0;
    }

    if (strcmp(banner, "dyntest") != 0) {
        printf("dyntest: FAIL this program's own global is wrong\n");
        return 1;
    }
    printf("dyntest: own global ok\n");

    char *p = malloc(64);
    if (!p) {
        printf("dyntest: FAIL malloc through libc.so\n");
        return 2;
    }
    strcpy(p, "through the PLT");
    if (strlen(p) != 15) {
        printf("dyntest: FAIL strlen through libc.so\n");
        return 3;
    }
    free(p);
    printf("dyntest: libc.so reached through the PLT\n");

    extern char **environ;
    if (environ == 0) {
        printf("dyntest: FAIL a libc.so global read as null\n");
        return 4;
    }
    printf("dyntest: a libc.so global read through the GOT\n");

    void *h = dlopen("libdyn.so", RTLD_NOW);
    if (!h) {
        printf("dyntest: FAIL dlopen: %s\n", dlerror());
        return 5;
    }
    int (*answer)(void) = (int (*)(void))dlsym(h, "dyn_answer");
    if (!answer) {
        printf("dyntest: FAIL dlsym: %s\n", dlerror());
        return 6;
    }
    if (answer() != 42) {
        printf("dyntest: FAIL the dlopened function returned the wrong value\n");
        return 7;
    }
    printf("dyntest: dlopen and dlsym of a library built after this program\n");
    dlclose(h);

    printf("dyntest: every check passed\n");
    return 0;
}
