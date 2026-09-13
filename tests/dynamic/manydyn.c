#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define N 24

int main(void) {
    void *handles[N];
    char path[128];
    char sym[64];

    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof(path), "/lib/many/manylib%d.so", i);
        handles[i] = dlopen(path, RTLD_NOW);
        if (!handles[i]) {
            printf("manydyn: FAIL dlopen %s: %s\n", path, dlerror());
            return 1;
        }
    }
    printf("manydyn: %d objects open at once\n", N);

    for (int i = 0; i < N; i++) {
        snprintf(sym, sizeof(sym), "many_answer_%d", i);
        int (*fn)(void) = (int (*)(void))dlsym(handles[i], sym);
        if (!fn) {
            printf("manydyn: FAIL dlsym %s: %s\n", sym, dlerror());
            return 2;
        }
        int want = (int)strlen("manylib") * i + i;
        int got = fn();
        if (got != want) {
            printf("manydyn: FAIL %s returned %d, wanted %d\n", sym, got, want);
            return 3;
        }
    }
    printf("manydyn: every one of the %d answered for itself\n", N);

    if (dlopen("/lib/many/nosuchlib.so", RTLD_NOW) != 0) {
        printf("manydyn: FAIL a pathname that does not exist was opened\n");
        return 4;
    }
    printf("manydyn: a pathname that is not there fails as one\n");

    printf("manydyn: every check passed\n");
    return 0;
}
