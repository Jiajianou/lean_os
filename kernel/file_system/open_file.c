#include "open_file.h"

#include "drivers/kernel_log.h"
#include "library/spinlock.h"
#include "memory_management/heap.h"
#include "panic.h"
#include "virtual_file_system.h"

/* From the kernel heap on first use rather than in the image: 4096 entries
   of about 280 bytes is 1.1 MB, and M187 found out what that costs in BSS -
   the loader could no longer reserve the kernel's load address and the
   machine did not boot. */
static open_file_t *table;
static int initialized;

static spinlock_t open_file_lock;

static int open_file_exhaustions;

/* Called before the lock is taken, because the heap is no place to go with
   interrupts off; the table is built outside it and published inside it, so
   two processors arriving together cannot both install one. */
static void ensure_init(void) {
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE)) {
        return;
    }
    open_file_t *fresh = (open_file_t *)kmalloc(sizeof(open_file_t) * MAX_OPEN_FILES);
    if (!fresh) {
        panic("open_file: no memory for the machine's table of open files");
    }
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        fresh[i].handle = -1;
    }
    uint64_t flags = spin_lock_irqsave(&open_file_lock);
    if (!initialized) {
        table = fresh;
        fresh = 0;
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&open_file_lock, flags);
    if (fresh) {
        kfree(fresh);
    }
}

open_file_t *open_file_alloc(int handle, int writable, const char *path, int is_directory) {
    ensure_init();
    uint64_t flags = spin_lock_irqsave(&open_file_lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle < 0) {
            table[i].handle = handle;
            table[i].offset = 0;
            table[i].writable = (uint8_t)(writable != 0);
            table[i].refcount = 1;
            table[i].is_directory = (uint8_t)(is_directory != 0);
            table[i].synchronous = 0;
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
            if (!is_directory) {
                virtual_file_system_handle_hold(handle);
            }
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
    int was_directory = f->is_directory;
    if (f->refcount > 0 && --f->refcount == 0) {
        closing = f->handle;
        f->handle = -1;
    }
    spin_unlock_irqrestore(&open_file_lock, flags);

    if (closing >= 0) {
        virtual_file_system_handle_close(closing);
        if (!was_directory) {
            virtual_file_system_handle_release(closing);
        }
    }
}

int open_file_in_use(void) {
    ensure_init();
    uint64_t flags = spin_lock_irqsave(&open_file_lock);
    int n = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle >= 0) {
            n++;
        }
    }
    spin_unlock_irqrestore(&open_file_lock, flags);
    return n;
}
