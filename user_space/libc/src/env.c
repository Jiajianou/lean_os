#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char **environ;

extern void __lean_stdio_flush_all(void);

static int environ_is_ours;
static int environ_cap;

extern void _init(void) __attribute__((weak));
extern void _fini(void) __attribute__((weak));
extern void (*__init_array_start[])(int, char **, char **) __attribute__((weak));
extern void (*__init_array_end[])(int, char **, char **) __attribute__((weak));
extern void (*__fini_array_start[])(void) __attribute__((weak));
extern void (*__fini_array_end[])(void) __attribute__((weak));

#define ATEXIT_MAX 32

typedef struct {
    void (*function)(void *);
    void *arg;
    void *dso;
    int takes_argument;
} exit_entry_t;

static exit_entry_t atexit_fns[ATEXIT_MAX];
static int atexit_count;

static int register_exit(void (*function)(void *), void *arg, void *dso, int takes_argument) {
    if (!function || atexit_count >= ATEXIT_MAX) {
        return -1;
    }
    atexit_fns[atexit_count].function = function;
    atexit_fns[atexit_count].arg = arg;
    atexit_fns[atexit_count].dso = dso;
    atexit_fns[atexit_count].takes_argument = takes_argument;
    atexit_count++;
    return 0;
}

int atexit(void (*function)(void)) {
    return register_exit((void (*)(void *))(void *)function, (void *)0, (void *)0, 0);
}

int __cxa_atexit(void (*function)(void *), void *arg, void *dso) {
    return register_exit(function, arg, dso, 1);
}

void __cxa_finalize(void *dso) {
    for (int i = atexit_count - 1; i >= 0; i--) {
        if (i >= atexit_count) {
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
        if (e.takes_argument) {
            e.function(e.arg);
        } else {
            ((void (*)(void))(void *)e.function)();
        }
    }
}

void __lean_run_exit_handlers(void) {
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

extern void *__lean_tls_setup(void);
extern void __lean_stack_protector_init(void);
extern void __lean_run_thread_destructors(void);

int __lean_start(int argc, char **argv, char **envp,
                 int (*mainfn)(int, char **, char **)) {
    (void)__lean_tls_setup();
    /* Before anything else, because every function after this one may be
       compiled with -fstack-protector and would then be checking a guard
       that had not been set. */
    __lean_stack_protector_init();
    environ = envp;
    if (argc > 0 && argv && argv[0]) {
        setprogname(argv[0]);
    }
    if (_init) {
        _init();
    }
    if (__init_array_start && __init_array_end) {
        for (void (**p)(int, char **, char **) = __init_array_start;
             p < __init_array_end; p++) {
            (*p)(argc, argv, envp);
        }
    }
    int rc = mainfn(argc, argv, envp);
    __lean_run_thread_destructors();
    __lean_run_exit_handlers();
    __lean_stdio_flush_all();
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

static int name_length(const char *entry) {
    const char *eq = strchr(entry, '=');
    return eq ? (int)(eq - entry) : -1;
}

static int matches(const char *entry, const char *name, size_t namelen) {
    return strncmp(entry, name, namelen) == 0 && entry[namelen] == '=';
}

static int own_environ(int extra) {
    int n = env_count();
    if (environ_is_ours && n + extra + 1 <= environ_cap) {
        return 0;
    }
    int cap = n + extra + 8;
    char **fresh = (char **)malloc((size_t)cap * sizeof(char *));
    if (!fresh) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        fresh[i] = environ[i];
    }
    fresh[n] = 0;
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
        return -1;
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

    size_t length = namelen + 1 + strlen(value) + 1;
    char *entry = (char *)malloc(length);
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
        return 0;
    }
    if (own_environ(0) != 0) {
        return -1;
    }
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

int putenv(char *entry) {
    if (!entry) {
        return -1;
    }
    int nl = name_length(entry);
    if (nl <= 0) {
        return -1;
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
