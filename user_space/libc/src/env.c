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
int __lean_start(int argc, char **argv, char **envp) {
    environ = envp;
    return main(argc, argv, envp);
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
