#include "openfile.h"

#include "lib/spinlock.h"
#include "vfs.h"

static openfile_t table[MAX_OPEN_FILES];
static int initialized;

static spinlock_t openfile_lock;

static void ensure_init(void) {
    if (initialized) {
        return;
    }
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        table[i].handle = -1;
    }
    initialized = 1;
}

openfile_t *openfile_alloc(int handle, int writable, const char *path, int is_dir) {
    uint64_t flags = spin_lock_irqsave(&openfile_lock);
    ensure_init();
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle < 0) {
            table[i].handle = handle;
            table[i].offset = 0;
            table[i].writable = (uint8_t)(writable != 0);
            table[i].refcount = 1;
            table[i].is_dir = (uint8_t)(is_dir != 0);
            table[i].path[0] = 0;
            if (path) {
                int n = 0;
                while (path[n] && n < OPENFILE_PATH_MAX - 1) {
                    n++;
                }
                if (!path[n]) {
                    for (int j = 0; j <= n; j++) {
                        table[i].path[j] = path[j];
                    }
                }
            }
            spin_unlock_irqrestore(&openfile_lock, flags);
            return &table[i];
        }
    }
    spin_unlock_irqrestore(&openfile_lock, flags);
    return 0;
}

void openfile_ref(openfile_t *f) {
    if (!f) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&openfile_lock);
    f->refcount++;
    spin_unlock_irqrestore(&openfile_lock, flags);
}

void openfile_unref(openfile_t *f) {
    if (!f) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&openfile_lock);
    int closing = -1;
    if (f->refcount > 0 && --f->refcount == 0) {
        closing = f->handle;
        f->handle = -1;
    }
    spin_unlock_irqrestore(&openfile_lock, flags);

    if (closing >= 0) {
        vfs_handle_close(closing);
    }
}

int openfile_in_use(void) {
    uint64_t flags = spin_lock_irqsave(&openfile_lock);
    ensure_init();
    int n = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle >= 0) {
            n++;
        }
    }
    spin_unlock_irqrestore(&openfile_lock, flags);
    return n;
}
