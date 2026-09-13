#include "application_dispatch.h"

const char *application_dispatch_name(const char *path) {
    if (!path) {
        return "";
    }
    const char *name = path;
    for (const char *at = path; *at; at++) {
        if (*at == '/') {
            name = at + 1;
        }
    }
    return name;
}

int application_dispatch_index(const char *path, const char *const *names, int count) {
    if (!names || count <= 0) {
        return -1;
    }
    const char *name = application_dispatch_name(path);
    if (!name[0]) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        const char *candidate = names[i];
        if (!candidate) {
            continue;
        }
        const char *a = candidate;
        const char *b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return -1;
}
