#include "open_file.h"

#include "library/spinlock.h"
#include "virtual_file_system.h"

static open_file_t table[MAX_OPEN_FILES];
static int initialized;

static spinlock_t open_file_lock;

static void ensure_init(void) {
    if (initialized) {
        return;
    }
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        table[i].handle = -1;
    }
    initialized = 1;
}

open_file_t *open_file_alloc(int handle, int writable, const char *path, int is_directory) {
    uint64_t flags = spin_lock_irqsave(&open_file_lock);
    ensure_init();
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle < 0) {
            table[i].handle = handle;
            table[i].offset = 0;
            table[i].writable = (uint8_t)(writable != 0);
            table[i].refcount = 1;
            table[i].is_directory = (uint8_t)(is_directory != 0);
            table[i].path[0] = 0;
            if (path) {
                int n = 0;
                while (path[n] && n < OPEN_FILE_PATH_MAX - 1) {
                    n++;
                }
                if (!path[n]) {
                    for (int j = 0; j <= n; j++) {
                        table[i].path[j] = path[j];
                    }
                }
            }
            spin_unlock_irqrestore(&open_file_lock, flags);
            return &table[i];
        }
    }
    spin_unlock_irqrestore(&open_file_lock, flags);
    return 0;
}

void open_file_reference(open_file_t *f) {
    if (!f) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&open_file_lock);
    f->refcount++;
    spin_unlock_irqrestore(&open_file_lock, flags);
}

void open_file_unref(open_file_t *f) {
    if (!f) {
        return;
    }
    uint64_t flags = spin_lock_irqsave(&open_file_lock);
    int closing = -1;
    if (f->refcount > 0 && --f->refcount == 0) {
        closing = f->handle;
        f->handle = -1;
    }
    spin_unlock_irqrestore(&open_file_lock, flags);

    if (closing >= 0) {
        virtual_file_system_handle_close(closing);
    }
}

int open_file_in_use(void) {
    uint64_t flags = spin_lock_irqsave(&open_file_lock);
    ensure_init();
    int n = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle >= 0) {
            n++;
        }
    }
    spin_unlock_irqrestore(&open_file_lock, flags);
    return n;
}
