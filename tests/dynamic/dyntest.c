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

    /* RTLD_NOLOAD before anything has loaded it: the answer is "not here",
       and that is an answer rather than a failure. M159 - ANGLE asks this to
       find out whether a shared library is present without bringing it in,
       and a dlopen that ignored the flag would LOAD it and report success,
       which is the opposite of what was asked. */
    if (dlopen("libdyn.so", RTLD_NOLOAD) != 0) {
        printf("dyntest: FAIL RTLD_NOLOAD found a library nothing had loaded\n");
        return 11;
    }

    void *h = dlopen("libdyn.so", RTLD_NOW);
    if (!h) {
        printf("dyntest: FAIL dlopen: %s\n", dlerror());
        return 5;
    }

    /* And after it is loaded, the same question finds the same object. */
    void *already = dlopen("libdyn.so", RTLD_NOLOAD);
    if (already != h) {
        printf("dyntest: FAIL RTLD_NOLOAD returned %p for a library loaded "
               "at %p\n", already, h);
        return 12;
    }
    if (dlopen("libnosuchthing.so", RTLD_NOLOAD) != 0) {
        printf("dyntest: FAIL RTLD_NOLOAD found a library that does not "
               "exist\n");
        return 13;
    }
    printf("dyntest: RTLD_NOLOAD answers before and after a load\n");
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
