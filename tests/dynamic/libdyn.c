/* tests/dynamic/libdyn.c - M95's dlopen fixture.
 *
 * Built AFTER dyntest and never named on its link line. The only way
 * dyntest can call this is by asking for it by name at run time, which
 * is the thing dlopen exists for and the thing static linking cannot
 * imitate.
 *
 * It calls into libc.so itself, so loading it exercises the case that
 * matters: a newly loaded object's relocations resolving against an
 * object that was already there.
 */
#include <string.h>

int dyn_answer(void);

int dyn_answer(void) {
    /* Through libc.so rather than a constant, so that a dlopen which
     * mapped the object without relocating it fails here rather than
     * returning the right number by accident. */
    static const char six[] = "424242";
    return (int)strlen(six) * 7;
}
