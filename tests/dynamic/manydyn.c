/* tests/dynamic/manydyn.c - M99: the loader past M95's ceiling.
 *
 * M95 set MAX_OBJECTS to sixteen and said why: "this machine's programs
 * link against libc.so and nothing else". Nothing here had ever opened
 * two, so sixteen was never a tested number - it was a comment. M99
 * needed it to be ninety-six, because an interpreter opens one object
 * per C extension module it imports, and dlclose's own note names that
 * as the condition for raising it.
 *
 * Twenty-four objects: past sixteen, inside ninety-six. That interval is
 * the only place a test can tell the old ceiling from the new one - at
 * eight it passes either way, and at a hundred it fails either way.
 *
 * And they are opened by FULL PATH out of /lib/many, which is not on the
 * loader's search list. That is the other half of the same milestone:
 * open_lib() searched LD_LIBRARY_PATH, then /lib, then /usr/lib, and
 * never tried the name it was given, so an absolute path became
 * "/lib//lib/many/manylib0.so" and the loader reported "cannot find" a
 * file that was sitting exactly where it had been told. CPython's
 * dynload_shlib.c hands dlopen the full path of every module it imports,
 * so this was going to be found either by twelve lines here or by an
 * interpreter forty minutes into a build.
 */
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

    /* Resolved AFTER all of them are loaded, so that a loader which
     * overwrote an earlier slot on the seventeenth open is caught here
     * rather than passing because each was checked while it was still
     * the newest thing in the table. */
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

    /* A path that is not there must fail as a path, and say so - rather
     * than falling back to a search that finds a same-named object
     * somewhere else. dlopen returning non-null here would mean the
     * loader treated a pathname as a hint. */
    if (dlopen("/lib/many/nosuchlib.so", RTLD_NOW) != 0) {
        printf("manydyn: FAIL a pathname that does not exist was opened\n");
        return 4;
    }
    printf("manydyn: a pathname that is not there fails as one\n");

    printf("manydyn: every check passed\n");
    return 0;
}
