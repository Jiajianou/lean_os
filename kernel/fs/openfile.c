#include "openfile.h"

#include "lib/spinlock.h"
#include "vfs.h" /* M101 - vfs_handle_close, at the last unref */

static openfile_t table[MAX_OPEN_FILES];
static int initialized;

/* ---- M67: openfile_lock ----------------------------------------------
 *
 * What it protects: the `table` above, and every refcount in it.
 *
 * Against whom: SYS_open racing SYS_open (two callers both seeing the
 * same `handle < 0` slot free), and SYS_close racing SYS_dup2 on the
 * same entry - `--f->refcount` is a read-modify-write, and losing one
 * decrement leaks the entry forever while losing an increment frees a
 * file another descriptor is still using. Neither was reachable before
 * M67, because `int 0x80` ran with interrupts off and a process has one
 * thread of control; with a trap gate the second CPU no longer needs the
 * process's cooperation to be in here at the same time.
 *
 * Interrupts off, for the same reason as fs_lock and shm_lock: the
 * task-exit path closes descriptors, and the task-exit path is reachable
 * from a timer tick.
 *
 * The offset inside a live entry is deliberately NOT protected here -
 * that is the *file position*, shared by every fd dup2 made from one
 * open, and two descriptors advancing one position between them is the
 * documented behaviour rather than a race. What must not tear is the
 * table's own allocation state, which is what this covers. */
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

openfile_t *openfile_alloc(int handle, int writable) {
    uint64_t flags = spin_lock_irqsave(&openfile_lock);
    ensure_init();
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].handle < 0) {
            table[i].handle = handle;
            table[i].offset = 0;
            table[i].writable = (uint8_t)(writable != 0);
            table[i].refcount = 1;
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

    /* M101: tell the filesystem, and do it with openfile_lock DROPPED.
     *
     * vfs_handle_close takes fs_lock. Calling it from inside this one
     * would establish openfile_lock -> fs_lock as a lock order, and
     * every other path in this kernel that touches both takes fs_lock
     * first (SYS_open: resolve the path, then claim a table entry). Two
     * orders is a deadlock waiting for the second CPU that M67 made
     * possible, so the entry is released first and the filesystem is
     * told second. Nothing observes the gap: the slot is already free
     * and `closing` is a handle no other descriptor names, which is
     * precisely what a refcount reaching zero means. */
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
