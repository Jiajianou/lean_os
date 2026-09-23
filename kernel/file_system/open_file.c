#include "open_file.h"

#include "drivers/kernel_log.h"
#include "library/spinlock.h"
#include "virtual_file_system.h"

static open_file_t table[MAX_OPEN_FILES];
static int initialized;

static spinlock_t open_file_lock;

static int open_file_exhaustions;

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
    /* A machine-wide ceiling reached is a fact about the MACHINE, and the
       caller only ever sees "open failed" - which looks exactly like a file
       that is not there. M183 spent an afternoon on the far end of that:
       Chromium's launcher opens /dev/null in the forked child before it
       execs, gets nothing, and _exit(127)s, so every child died at birth
       with no explanation anywhere. Reported once per boot and then every
       64th time, because a machine at its ceiling stays at it. */
    open_file_exhaustions++;
    if (open_file_exhaustions == 1 || open_file_exhaustions % 64 == 0) {
        kernel_log_puts("[openfile] the machine is at its ceiling of ");
        kernel_log_put_dec(MAX_OPEN_FILES);
        kernel_log_puts(" open files - refusing to open ");
        kernel_log_puts(path ? path : "(no path)");
        kernel_log_puts(" (");
        kernel_log_put_dec((uint32_t)open_file_exhaustions);
        kernel_log_puts(" refusal(s) since boot)\n");
    }
    return 0;
}

int open_file_exhaustion_count(void) {
    return open_file_exhaustions;
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
