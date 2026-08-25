#include "openfile.h"

static openfile_t table[MAX_OPEN_FILES];
static int initialized;

static void ensure_init(void) {
    if (initialized) {
        return;
    }
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        table[i].handle = -1;
    }
    initialized = 1;
}

openfile_t *openfile_alloc(int handle, int writable) {
    ensure_init();
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle < 0) {
            table[i].handle = handle;
            table[i].offset = 0;
            table[i].writable = (uint8_t)(writable != 0);
            table[i].refcount = 1;
            return &table[i];
        }
    }
    return 0;
}

void openfile_ref(openfile_t *f) {
    if (f) {
        f->refcount++;
    }
}

void openfile_unref(openfile_t *f) {
    if (!f || f->refcount <= 0) {
        return;
    }
    if (--f->refcount == 0) {
        f->handle = -1;
    }
}

int openfile_in_use(void) {
    ensure_init();
    int n = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle >= 0) {
            n++;
        }
    }
    return n;
}
