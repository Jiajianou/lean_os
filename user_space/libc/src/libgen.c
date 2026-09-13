#include <libgen.h>

static char dot[] = ".";
static char slash[] = "/";

char *basename(char *path) {
    if (!path || !*path) {
        return dot;
    }
    char *end = path;
    while (*end) {
        end++;
    }
    while (end > path && end[-1] == '/') {
        end--;
    }
    if (end == path) {
        return slash;
    }
    *end = '\0';
    char *start = end;
    while (start > path && start[-1] != '/') {
        start--;
    }
    return start;
}

char *dirname(char *path) {
    if (!path || !*path) {
        return dot;
    }
    char *end = path;
    while (*end) {
        end++;
    }
    while (end > path && end[-1] == '/') {
        end--;
    }
    if (end == path) {
        return slash;
    }
    while (end > path && end[-1] != '/') {
        end--;
    }
    if (end == path) {
        return dot;
    }
    while (end > path && end[-1] == '/') {
        end--;
    }
    if (end == path) {
        return slash;
    }
    *end = '\0';
    return path;
}
