/* user_space/libc/include/dlfcn.h - M95
 *
 * Loading a shared object by name at run time.
 *
 * **The implementations are not in libc.** They are in the dynamic
 * linker (user_space/ld/ld-lean.c), because they are its data
 * structures: which objects are loaded, where, and in what search order.
 * A libc copy would be a second loader that had to agree with the first
 * about all of it.
 *
 * That means a program using these links against `ld-lean.so`, which is
 * already in its search scope - the linker adds itself, last, so that
 * these four names are findable and nothing else resolves against it by
 * accident.
 *
 * A STATIC program cannot use them at all, and gets a link error rather
 * than a stub that fails at run time. That is the honest answer: there
 * is no loader in a static image to ask.
 */
#pragma once

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

/* RTLD_LAZY is accepted and behaves as RTLD_NOW: binding is eager
 * throughout this linker (see its header for why lazy binding is a
 * refusal rather than an omission), and a lazy binding that happens
 * early is still a correct one. */
#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL  0x0000

/* NULL `file` is a handle for the main program. */
void *dlopen(const char *file, int flags);
void *dlsym(void *handle, const char *name);
/* Does NOT unmap: see the linker's note on why an object that a program
 * may still hold a pointer into stays where it is. */
int   dlclose(void *handle);
/* The last error, cleared by reading it. NULL when there has been none
 * since the last call. */
char *dlerror(void);

#ifdef __cplusplus
}
#endif
