#include <execinfo.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct _Unwind_Context;
typedef int (*unwind_trace_function)(struct _Unwind_Context *context, void *argument);
extern int _Unwind_Backtrace(unwind_trace_function function, void *argument);
extern uintptr_t _Unwind_GetIP(struct _Unwind_Context *context);

enum {
    UNWIND_NO_REASON = 0,
    UNWIND_END_OF_STACK = 5,
};

typedef struct {
    void **buffer;
    int size;
    int count;
    int skipped_self;
} backtrace_walk_t;

static int backtrace_step(struct _Unwind_Context *context, void *argument) {
    backtrace_walk_t *walk = (backtrace_walk_t *)argument;
    if (!walk->skipped_self) {
        walk->skipped_self = 1;
        return UNWIND_NO_REASON;
    }
    if (walk->count >= walk->size) {
        return UNWIND_END_OF_STACK;
    }
    uintptr_t address = _Unwind_GetIP(context);
    if (address == 0) {
        return UNWIND_END_OF_STACK;
    }
    walk->buffer[walk->count++] = (void *)address;
    return UNWIND_NO_REASON;
}

int backtrace(void **buffer, int size) {
    if (!buffer || size <= 0) {
        return 0;
    }
    backtrace_walk_t walk = {buffer, size, 0, 0};
    _Unwind_Backtrace(backtrace_step, &walk);
    return walk.count;
}

static int describe_frame(void *address, char *out, size_t length) {
    Dl_info info;
    if (dladdr(address, &info) && info.dli_sname && info.dli_saddr) {
        return snprintf(out, length, "%s(%s+0x%lx) [%p]",
                        info.dli_fname ? info.dli_fname : "",
                        info.dli_sname,
                        (unsigned long)((uintptr_t)address - (uintptr_t)info.dli_saddr),
                        address);
    }
    return snprintf(out, length, "[%p]", address);
}

char **backtrace_symbols(void *const *buffer, int size) {
    if (!buffer || size <= 0) {
        return (char **)0;
    }
    size_t text = 0;
    char line[512];
    for (int i = 0; i < size; i++) {
        int n = describe_frame(buffer[i], line, sizeof(line));
        text += (size_t)(n < 0 ? 0 : (n >= (int)sizeof(line) ? (int)sizeof(line) - 1 : n)) + 1;
    }
    size_t table = (size_t)size * sizeof(char *);
    char **result = (char **)malloc(table + text);
    if (!result) {
        return (char **)0;
    }
    char *cursor = (char *)result + table;
    char *end = cursor + text;
    for (int i = 0; i < size; i++) {
        describe_frame(buffer[i], cursor, (size_t)(end - cursor));
        result[i] = cursor;
        cursor += strlen(cursor) + 1;
    }
    return result;
}

void backtrace_symbols_fd(void *const *buffer, int size, int fd) {
    char line[512];
    for (int i = 0; buffer && i < size; i++) {
        int n = describe_frame(buffer[i], line, sizeof(line) - 1);
        if (n < 0) {
            continue;
        }
        if (n > (int)sizeof(line) - 2) {
            n = (int)sizeof(line) - 2;
        }
        line[n] = '\n';
        if (write(fd, line, (size_t)n + 1) < 0) {
            return;
        }
    }
}
