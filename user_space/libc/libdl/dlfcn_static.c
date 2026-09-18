#include <dlfcn.h>
#include <stddef.h>

/* dlopen, dlsym, dlclose and dlerror for a STATICALLY linked program.
 *
 * The real four live in the dynamic loader (user_space/loader/ld-lean.c),
 * because only a program the loader started has anything for them to answer
 * about. A program linked statically has no loader behind it and no dynamic
 * symbol table to search, so the four have no way to succeed - and this is
 * what libdl.a holds for exactly that case.
 *
 * It is a separate archive rather than part of libc.a on purpose. A
 * dynamically linked program resolves these from ld-lean.so at run time, and
 * a definition in libc.a would be bound at link time instead and silently
 * win - which would take dlopen away from the programs that have it. -ldl is
 * already on every link line that asks, so the two never meet.
 *
 * Chromium's base::NativeLibrary is what asked (M164). A static Chromium
 * never loads a shared object; what it needs is for the four names to
 * resolve, and for the answer to be no rather than a crash.
 *
 * The condition for these becoming real is the one CLAUDE.md already
 * records against dynamic linking: a statically linked program cannot grow a
 * loader, so the answer here does not change - what changes is which of the
 * two archives a program is linked against.
 */

static const char *last_error;

static const char kNoLoader[] =
    "this program is statically linked and has no dynamic loader";

void *dlopen(const char *file, int flags) {
    (void)file;
    (void)flags;
    last_error = kNoLoader;
    return NULL;
}

void *dlsym(void *handle, const char *name) {
    (void)handle;
    (void)name;
    last_error = kNoLoader;
    return NULL;
}

int dlclose(void *handle) {
    (void)handle;
    last_error = kNoLoader;
    return -1;
}

/* dlerror(3) reports the error since the LAST call to dlerror and then
   forgets it, so two calls with no failure between them give a message and
   then a null pointer. A version that kept returning the same string would
   make a caller that loops until dlerror() is null loop for ever. */
char *dlerror(void) {
    const char *error = last_error;
    last_error = NULL;
    return (char *)error;
}
