/* tests/cxx/throwmain.cpp - M97: the near side of the boundary.
 *
 * A position-independent executable that dlopens tests/cxx/throwlib.cpp
 * - a shared object it never named on its link line - and:
 *
 *   1. calls into it, catches by exact type the exception it threw, and
 *      checks that the destructors of the frames INSIDE the shared
 *      object ran on the way out. That last part is the whole test:
 *      a catch that fires while skipping a destructor looks like
 *      success, and M97's own bullet says so.
 *   2. catches a derived object by its base, which needs the ABI's type
 *      walk rather than a pointer compare.
 *   3. throws from here INTO a catch inside the shared object, so the
 *      boundary is crossed in the other direction as well.
 *
 * Exit codes:
 *   0  everything worked
 *   2  dlopen or dlsym failed
 *   3  the exception was not caught, or was caught by the wrong type
 *   4  the value did not survive the boundary
 *   5  a destructor inside the shared object did not run
 *   6  catching by base across the boundary failed
 *   7  a throw from here was not caught inside the shared object
 */
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
    /* By SONAME, not by path - the loader searches for it the same way
     * it searched for libc.so and libstdc++.so.6, which is the point:
     * a program should not have to know where a library lives. */
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

    /* ---- 1. out of the shared object, caught by exact type ---------- */
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
    /* Two frames inside the shared object, two destructors. If the
     * unwinder could not find that object's .eh_frame it would have
     * jumped straight here and this would be 0. */
    if (destroyed != 2) {
        printf("throwmain: %d destructors ran inside the shared object, "
               "expected 2\n", destroyed);
        return 5;
    }

    /* ---- 2. a derived object, caught by its BASE -------------------
     *
     * The shared object throws LibDerived; this catches LibError. That
     * cannot be a pointer comparison between two type_info objects - it
     * is the ABI's __do_catch walking the inheritance chain, using type
     * identity that has to hold across the boundary at every step. It is
     * the check M97's bullet means by "RTTI ... need type identity to be
     * stable across shared objects ... and both get it wrong in the same
     * way". */
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

    /* ---- 3. and the other direction --------------------------------
     *
     * A throw raised HERE, in the executable, unwinding into a catch
     * clause compiled into the shared object. That needs the shared
     * object's own .gcc_except_table to be readable by the unwinder,
     * which is a different table from the .eh_frame the first two checks
     * proved. Both halves of a boundary crossing, rather than the
     * easier one twice. */
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
