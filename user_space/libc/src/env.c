/* user_space/libc/src/env.c - M75
 *
 * The environment, and the startup shim that publishes it.
 *
 * Until this milestone SYS_spawn carried an argv and nothing else, so
 * `getenv` could not exist: there was nowhere for a value to have come
 * from. The kernel now lays an envp vector into every process's argument
 * region immediately after argv (see kernel/proc/proc.h), crt0 hands
 * both to __lean_start below, and this file is what turns that vector
 * into the `environ` every C program expects to find.
 *
 * Two representations, deliberately:
 *
 *  - at startup, `environ` points straight into the kernel's argument
 *    region. Nothing is copied, nothing is allocated, and a program that
 *    only ever *reads* its environment - which is almost all of them -
 *    pays nothing at all.
 *  - the first setenv/unsetenv/putenv moves it onto the heap, because
 *    the kernel's region has no room to grow into and is exactly the
 *    size the parent chose. From then on the array is ours, and grows by
 *    reallocation the ordinary way.
 *
 * The one thing this does NOT do is free strings it stops pointing at.
 * `putenv` hands us a caller's buffer whose lifetime is the caller's,
 * and a `setenv` that freed the previous value would free that buffer
 * out from under a program that is entitled to keep using it - which is
 * why every real libc leaks here too, and says so.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char **environ;

/* Set once `environ` points at storage this file owns. Before that it
 * points into the kernel's argument region, which must not be freed,
 * grown, or written past. */
static int environ_is_ours;
static int environ_cap; /* slots in the owned array, terminator included */

extern int main(int argc, char **argv, char **envp);

/* crt0 calls this instead of main. The whole reason it exists is the
 * assignment below: `environ` has to be live before main's first line,
 * and a program that declares `int main(void)` cannot be the one to set
 * it. main is called with three arguments regardless of how it was
 * declared - SysV puts the extras in registers a two- or zero-argument
 * main simply never reads - so `int main(int argc, char **argv, char
 * **envp)` works here too, which is what a program written for a real
 * Unix expects. */
/* M94: the constructor and destructor arrays, and _init/_fini.
 *
 * Both mechanisms exist and both are walked, because a program is
 * compiled by whatever compiler the person had: an object built by a
 * modern GCC puts a pointer in .init_array, and one built by an older
 * toolchain (or with -fno-use-cxa-atexit) contributes a fragment to
 * .init instead. Walking one and not the other is a constructor that
 * silently does not run, which is the failure this is hardest to notice.
 *
 * The symbols come from user_space/lib/user.ld, and are PROVIDE_HIDDEN
 * there so that a program with no constructors links with the two
 * bounds equal rather than undefined. */
/* M95: weak, because in a DYNAMIC program none of them is visible from
 * here. These symbols are defined by user_space/lib/user.ld, which links
 * a static executable; a shared object is linked without it, and the
 * program's own arrays live in the executable where libc.so cannot see
 * them. The dynamic linker runs the program's initializers instead - it
 * has the program's DT_INIT_ARRAY, which is the same information by the
 * other route - so what this file must do in that case is nothing, and a
 * weak-undefined symbol resolving to 0 is how it finds out. */
extern void _init(void) __attribute__((weak));
extern void _fini(void) __attribute__((weak));
extern void (*__init_array_start[])(int, char **, char **) __attribute__((weak));
extern void (*__init_array_end[])(int, char **, char **) __attribute__((weak));
extern void (*__fini_array_start[])(void) __attribute__((weak));
extern void (*__fini_array_end[])(void) __attribute__((weak));

/* ---- M94: atexit ------------------------------------------------------
 *
 * <unistd.h>'s note on _exit said "nothing here registers anything yet -
 * `exit` is a syscall and no more... The distinction becomes real the
 * day this libc grows atexit". This is that day, and the distinction is
 * now observable: exit() runs these and _exit() does not, which is
 * exactly what a forked child that decides not to exec depends on.
 *
 * 32 slots is what POSIX requires at minimum (_POSIX_ATEXIT_MAX is 32)
 * and more than anything here registers. A registration past that is
 * refused with -1 rather than dropped, because a program that registers
 * a flush and is told it succeeded will not flush.
 */
#define ATEXIT_MAX 32

/* ---- M97: one list, two shapes of entry -------------------------------
 *
 * atexit takes `void (*)(void)`. The C++ ABI's __cxa_atexit takes
 * `void (*)(void *)` plus an argument and the handle of the shared object
 * the destructor belongs to, and that third field is the whole reason it
 * exists: a static object inside a shared library must be destroyed when
 * that library is unloaded, not when the program exits, or dlclose leaves
 * destructors pointing at unmapped code.
 *
 * One list rather than two, because the ORDER between them matters and
 * two lists cannot express it: `static Foo f;` and `atexit(g)` in the
 * same translation unit must unwind in reverse registration order
 * regardless of which mechanism registered which. Two lists would run all
 * of one and then all of the other, which is the wrong answer in a way
 * nothing would notice until a destructor read something g had freed.
 */
typedef struct {
    void (*fn)(void *);
    void *arg;
    void *dso;
    /* Which of the two shapes this is. A plain atexit handler is stored
     * with a NULL arg and called through a cast; keeping the distinction
     * explicit means the call site never has to guess from the arg. */
    int takes_arg;
} exit_entry_t;

static exit_entry_t atexit_fns[ATEXIT_MAX];
static int atexit_count;

/* The main executable's handle. GCC's crtbegin.o defines this on targets
 * whose crtstuff was configured to; ours does not (see
 * tools/toolchain-port/apply.py on which parts of crtstuff this target
 * builds), so libc supplies it. Its ADDRESS is the identity - the value
 * is never read - which is why it can be a single byte.
 *
 * The dynamic linker gives each shared object its own; a destructor
 * registered against this one belongs to the program and runs at exit. */
/* `__dso_handle` is NOT defined here, and where it comes from moved
 * during this milestone, which is worth recording because the first
 * version of this file did define it.
 *
 * GCC's crtstuff.c defines it - but only on a target configured with
 * `DEFAULT_USE_CXA_ATEXIT`, which is what M97 turned on. So under M94's
 * configuration there was nobody to define it and libc had to; under
 * M97's, crtbegin.o does, and libc defining it too is a duplicate symbol
 * at every C++ link. It is declared in <stdlib.h> and defined by the
 * startup files, which is where the rest of the world puts it. */

static int register_exit(void (*fn)(void *), void *arg, void *dso, int takes_arg) {
    if (!fn || atexit_count >= ATEXIT_MAX) {
        return -1;
    }
    atexit_fns[atexit_count].fn = fn;
    atexit_fns[atexit_count].arg = arg;
    atexit_fns[atexit_count].dso = dso;
    atexit_fns[atexit_count].takes_arg = takes_arg;
    atexit_count++;
    return 0;
}

int atexit(void (*fn)(void)) {
    return register_exit((void (*)(void *))(void *)fn, (void *)0, (void *)0, 0);
}

/* M97: the C++ ABI entry point. GCC emits a call to this for every
 * function-local static and every namespace-scope object with a
 * non-trivial destructor, once the target is configured with
 * `default_use_cxa_atexit=yes` - which this one now is, because the
 * alternative it falls back to (a per-translation-unit destructor
 * registered with plain atexit) cannot express "unload this library"
 * at all. */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    return register_exit(fn, arg, dso, 1);
}

/* M97: run and remove every destructor belonging to one shared object,
 * or all of them when `dso` is NULL - which is what exit() means.
 *
 * Reverse order, and the removal has to happen before the call for the
 * same reason __lean_run_exit_handlers' does: a destructor that triggers
 * another dlclose, or calls exit, must not find itself still on the
 * list. Compaction rather than a tombstone because the list is 32 entries
 * and walked backwards; a hole would have to be skipped by every later
 * pass and that is more state than this saves. */
void __cxa_finalize(void *dso) {
    for (int i = atexit_count - 1; i >= 0; i--) {
        if (i >= atexit_count) {
            /* A destructor ran and shortened the list under us. */
            i = atexit_count;
            continue;
        }
        if (dso && atexit_fns[i].dso != dso) {
            continue;
        }
        exit_entry_t e = atexit_fns[i];
        for (int k = i; k < atexit_count - 1; k++) {
            atexit_fns[k] = atexit_fns[k + 1];
        }
        atexit_count--;
        if (e.takes_arg) {
            e.fn(e.arg);
        } else {
            ((void (*)(void))(void *)e.fn)();
        }
    }
}

/* Called by exit(), in reverse order of registration - which is what the
 * standard requires and what makes nesting work: a handler registered by
 * another handler's setup runs before it. The destructor array and
 * _fini run after, in the order the ELF ABI specifies. */
void __lean_run_exit_handlers(void) {
    /* M97: everything, whichever way it was registered - see
     * __cxa_finalize, which is the same walk with a filter this call does
     * not want. */
    __cxa_finalize((void *)0);
    if (__fini_array_start && __fini_array_end) {
        for (void (**p)(void) = __fini_array_end; p > __fini_array_start;) {
            (*--p)();
        }
    }
    if (_fini) {
        _fini();
    }
}

/* M94: what this program is called - see <stdlib.h> for why both
 * spellings are here and which build asked for them. */
char *program_invocation_name = (char *)"";
char *program_invocation_short_name = (char *)"";

const char *getprogname(void) {
    return program_invocation_short_name;
}

void setprogname(const char *name) {
    if (!name) {
        return;
    }
    program_invocation_name = (char *)name;
    const char *slash = strrchr(name, '/');
    program_invocation_short_name = (char *)(slash ? slash + 1 : name);
}

/* M96: thread-local storage, before anything that might use it. */
extern void *__lean_tls_setup(void);

int __lean_start(int argc, char **argv, char **envp) {
    /* FIRST, before environ, before the program name, before any
     * constructor: `errno` is a `__thread` variable now, and every line
     * below this one might set it. A constructor that ran before the
     * thread pointer existed would write through %fs:0 with a base of
     * zero, which is a null dereference wearing a segment override. */
    (void)__lean_tls_setup();
    environ = envp;
    /* Before the constructors: a constructor that logs is entitled to
     * know the program's name, and there is nothing here that needs to
     * happen before this. */
    if (argc > 0 && argv && argv[0]) {
        setprogname(argv[0]);
    }
    /* _init first, then the array: that is the order every ELF runtime
     * uses, and it matters for an object that contributes to both. */
    if (_init) {
        _init();
    }
    if (__init_array_start && __init_array_end) {
        for (void (**p)(int, char **, char **) = __init_array_start;
             p < __init_array_end; p++) {
            (*p)(argc, argv, envp);
        }
    }
    int rc = main(argc, argv, envp);
    /* Falling off the end of main is a call to exit(), not to _exit() -
     * C says so, and it is why a program that returns from main still
     * gets its atexit handlers run. crt0 calls sys_exit with what this
     * returns, so the handlers have to run here. */
    __lean_run_exit_handlers();
    return rc;
}

static int env_count(void) {
    int n = 0;
    if (environ) {
        while (environ[n]) {
            n++;
        }
    }
    return n;
}

/* Length of the NAME part of "NAME=value", or -1 if there is no '='. */
static int name_len(const char *entry) {
    const char *eq = strchr(entry, '=');
    return eq ? (int)(eq - entry) : -1;
}

static int matches(const char *entry, const char *name, size_t namelen) {
    return strncmp(entry, name, namelen) == 0 && entry[namelen] == '=';
}

/* Moves `environ` onto the heap with room for `extra` more entries.
 * Returns 0, or -1 if there is no memory - in which case `environ` is
 * left exactly as it was, so a failed setenv changes nothing. */
static int own_environ(int extra) {
    int n = env_count();
    if (environ_is_ours && n + extra + 1 <= environ_cap) {
        return 0;
    }
    int cap = n + extra + 8; /* +8 so a run of setenv calls is not a run of reallocations */
    char **fresh = (char **)malloc((size_t)cap * sizeof(char *));
    if (!fresh) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        fresh[i] = environ[i];
    }
    fresh[n] = 0;
    /* The old array is freed only if it was ours; the startup one lives
     * in the kernel's argument region and is not heap memory at all. */
    if (environ_is_ours) {
        free(environ);
    }
    environ = fresh;
    environ_is_ours = 1;
    environ_cap = cap;
    return 0;
}

char *getenv(const char *name) {
    if (!name || !*name || !environ) {
        return 0;
    }
    size_t namelen = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (matches(environ[i], name, namelen)) {
            return environ[i] + namelen + 1;
        }
    }
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) {
        return -1; /* a name containing '=' would name a different variable */
    }
    if (!value) {
        value = "";
    }
    size_t namelen = strlen(name);
    int found = -1;
    for (int i = 0; environ && environ[i]; i++) {
        if (matches(environ[i], name, namelen)) {
            found = i;
            break;
        }
    }
    if (found >= 0 && !overwrite) {
        return 0;
    }

    size_t len = namelen + 1 + strlen(value) + 1;
    char *entry = (char *)malloc(len);
    if (!entry) {
        return -1;
    }
    memcpy(entry, name, namelen);
    entry[namelen] = '=';
    memcpy(entry + namelen + 1, value, strlen(value) + 1);

    if (own_environ(1) != 0) {
        free(entry);
        return -1;
    }
    if (found >= 0) {
        environ[found] = entry;
        return 0;
    }
    int n = env_count();
    environ[n] = entry;
    environ[n + 1] = 0;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) {
        return -1;
    }
    if (!environ) {
        return 0;
    }
    size_t namelen = strlen(name);
    int found = -1;
    for (int i = 0; environ[i]; i++) {
        if (matches(environ[i], name, namelen)) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        return 0; /* removing what was never there is a success, not an error */
    }
    if (own_environ(0) != 0) {
        return -1;
    }
    /* Re-find: own_environ may have moved the array. */
    for (int i = 0; environ[i]; i++) {
        if (matches(environ[i], name, namelen)) {
            int j = i;
            while (environ[j + 1]) {
                environ[j] = environ[j + 1];
                j++;
            }
            environ[j] = 0;
            break;
        }
    }
    return 0;
}

/* The caller's buffer becomes part of the environment - that is putenv's
 * whole (and much-criticised) contract, and changing it here would make
 * this a differently-named setenv rather than putenv. */
int putenv(char *entry) {
    if (!entry) {
        return -1;
    }
    int nl = name_len(entry);
    if (nl <= 0) {
        return -1; /* not "NAME=value" */
    }
    size_t namelen = (size_t)nl;
    for (int i = 0; environ && environ[i]; i++) {
        if (matches(environ[i], entry, namelen)) {
            if (own_environ(0) != 0) {
                return -1;
            }
            for (int j = 0; environ[j]; j++) {
                if (matches(environ[j], entry, namelen)) {
                    environ[j] = entry;
                    return 0;
                }
            }
            return -1;
        }
    }
    if (own_environ(1) != 0) {
        return -1;
    }
    int n = env_count();
    environ[n] = entry;
    environ[n + 1] = 0;
    return 0;
}

int clearenv(void) {
    if (own_environ(0) != 0) {
        return -1;
    }
    environ[0] = 0;
    return 0;
}
