/* tests/dynamic/dyntest.c - M95's fixture.
 *
 * A position-independent executable linked against libc.so rather than
 * libc.a. Nothing about the source says so: this is an ordinary C
 * program, and that is the point - "code that is loaded, not linked" is
 * a property of the build and of the machine, not of the program.
 *
 * What it checks, and why each is the thing that would be wrong:
 *
 *   - printf, strlen and malloc all reach libc.so through the PLT and
 *     the GOT. A JUMP_SLOT that was not filled in is a jump to a
 *     resolver stub that does not exist here (binding is eager - see
 *     user_space/ld/ld-lean.c), which is a fault rather than a wrong
 *     answer, so simply running proves the relocations were applied.
 *   - a global in libc.so, read through a GLOB_DAT, and a global in
 *     THIS program, read directly. The second is the one a bad load
 *     bias breaks and the first is the one a bad GOT breaks, and they
 *     fail differently.
 *   - dlopen of a library built AFTER this program, and a symbol
 *     resolved out of it by name. That is M95's own second test and it
 *     cannot be faked by static linking: the library did not exist when
 *     this was compiled.
 *
 * `argv[1]` is "share" for the second half of M95's first test - see
 * the [m95] self-test, which runs two copies at once and counts frames.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A global of this program's own, in .data. Reached without the GOT and
 * therefore wrong if the load bias is wrong. */
static char banner[] = "dyntest";

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "share") == 0) {
        /* The frame-count half: hold still long enough for the other
         * copy to be running at the same time, then leave. Nothing is
         * printed - what is being measured is on the kernel's side. */
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

    /* environ is a global defined in libc.so, referenced from here -
     * which is a GLOB_DAT (or a COPY relocation, depending on how the
     * linker decided) rather than a call. A different mechanism from
     * every check above. */
    extern char **environ;
    if (environ == 0) {
        printf("dyntest: FAIL a libc.so global read as null\n");
        return 4;
    }
    printf("dyntest: a libc.so global read through the GOT\n");

    /* ---- dlopen: a library built after this program -------------- */
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
