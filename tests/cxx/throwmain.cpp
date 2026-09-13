#include "throwlib.h"

extern "C" {
#include <dlfcn.h>
#include <stdio.h>
}

typedef void (*throw_fn)(int *, int);
typedef int (*catches_fn)(void (*)());

static void thrower_here() {
    throw LibError(31);
}

int main(void) {
    void *h = dlopen("libthrow.so", 0);
    if (!h) {
        printf("throwmain: dlopen failed: %s\n", dlerror());
        return 2;
    }
    throw_fn lib_throw_p = (throw_fn)dlsym(h, "lib_throw");
    catches_fn lib_catches_p = (catches_fn)dlsym(h, "lib_catches");
    if (!lib_throw_p || !lib_catches_p) {
        printf("throwmain: dlsym failed\n");
        return 2;
    }

    int destroyed = 0;
    int code = 0;
    try {
        lib_throw_p(&destroyed, 0);
        return 3;
    } catch (const LibError &e) {
        code = e.code;
    } catch (...) {
        return 3;
    }
    if (code != 7) {
        return 4;
    }
    if (destroyed != 2) {
        printf("throwmain: %d destructors ran inside the shared object, "
               "expected 2\n", destroyed);
        return 5;
    }

    destroyed = 0;
    code = 0;
    try {
        lib_throw_p(&destroyed, 1);
        return 6;
    } catch (const LibError &e) {
        code = e.code;
    } catch (...) {
        return 6;
    }
    if (code != 9 || destroyed != 2) {
        return 6;
    }

    int back = lib_catches_p(thrower_here);
    if (back != 31) {
        printf("throwmain: the shared object caught %d, expected 31\n", back);
        return 7;
    }

    printf("throwmain: an exception thrown inside a dlopen'd shared object "
           "caught by exact type and by base in the executable, with both of "
           "that object's frame destructors run; and one thrown here caught "
           "inside it\n");
    dlclose(h);
    return 0;
}
