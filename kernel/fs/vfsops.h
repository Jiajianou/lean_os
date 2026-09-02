/* kernel/fs/vfsops.h - M87
 *
 * The seam kernel/fs/vfs.h has described since M53 as "the seam a second
 * filesystem type would plug into if this project ever needed one", with
 * an honest note that a vtable would be "pure speculative generality for
 * a single-filesystem kernel".
 *
 * It is not a single-filesystem kernel any more. `/dev/null` is the most
 * common path a ported program touches that this machine could not
 * answer, and it is not a file on a disk - it is a rule. So is
 * `/proc/self/exe`. Neither can live in leanfs without leanfs learning
 * to lie about what a file is.
 *
 * What this is NOT: a full vnode layer with a reference-counted inode
 * cache. Every operation here is still keyed by path, exactly as vfs.c's
 * have always been, and the mount table is a short array searched by
 * longest prefix. That is enough for three filesystems and it is honest
 * about being enough for three - the thing M53 declined to build was a
 * dispatch mechanism with no second implementation, and this has two.
 *
 * The operations a synthetic filesystem must supply are the ones a
 * program actually performs on `/dev/null` and `/proc/uptime`: look at
 * it, open it, read it, write it, list the directory it is in. There is
 * deliberately no mkdir, unlink, rename or truncate in this table - a
 * caller that tries one gets -1 from vfs.c without the filesystem being
 * asked, because "you cannot create a file in /proc" is a property of
 * /proc rather than an operation it declines.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "leanfs.h" /* leanfs_stat_t, leanfs_dir_entry_t - the shapes every filesystem here reports in */

typedef struct vfs_ops {
    /* `rel` is the path with the mount's prefix removed, always starting
     * with '/' - so a mount at "/dev" sees "/null", and the mount point
     * itself is "/". */
    int (*stat)(const char *rel, leanfs_stat_t *out);
    int (*is_dir)(const char *rel);
    int (*exists)(const char *rel);

    /* Returns a handle local to this filesystem, or -1. vfs.c tags it
     * with the mount index before handing it out - see VFS_HANDLE_MAKE. */
    int (*open)(const char *rel, int create);
    int64_t (*read)(int handle, void *buf, size_t len, uint32_t off);
    int64_t (*write)(int handle, const void *buf, size_t len, uint32_t off);
    uint32_t (*size)(int handle);
    int (*handle_stat)(int handle, leanfs_stat_t *out);

    /* One entry at a time, same cookie contract as leanfs_readdir. */
    int (*readdir)(const char *rel, uint32_t *cookie, leanfs_dir_entry_t *out);

    /* M101: the last descriptor naming this handle has gone.
     *
     * Optional - NULL means the filesystem has nothing to give back, and
     * leanfs is genuinely in that position: its handle IS an inode index,
     * so there is no per-open state to release and there never was. That
     * is exactly why this hook did not exist, and exactly why its absence
     * was a bug the moment a synthetic filesystem with a fixed table of
     * open files was mounted. procfs allocates a slot in proc_open and,
     * until this, released it nowhere - so the seventeenth open of any
     * /proc file on a given boot failed, permanently, and nothing in this
     * tree noticed for six milestones. See M101's notes.
     *
     * Called by vfs_handle_close, which openfile_unref calls at
     * refcount 0 - so a file held open by two dup2'd descriptors is
     * closed once, when the second one goes. */
    void (*close)(int handle);
} vfs_ops_t;

/* ---- handles ----------------------------------------------------------
 *
 * A handle has to say which filesystem it belongs to, because the fd
 * table stores one and comes back much later asking to read it.
 *
 * The mount index goes in the top byte, which makes leanfs's handles -
 * mount 0 - numerically unchanged. That is not a coincidence, it is the
 * point: an inode index has been the value of a handle since M59, it is
 * stored in openfile_t and in every fd slot, and a scheme that renumbered
 * them would have been a change to something already on disk in a saved
 * session. */
#define VFS_HANDLE_MOUNT(h)   (((uint32_t)(h) >> 24) & 0xFFu)
#define VFS_HANDLE_LOCAL(h)   ((int)((uint32_t)(h) & 0x00FFFFFFu))
#define VFS_HANDLE_MAKE(m, l) ((int)((((uint32_t)(m)) << 24) | ((uint32_t)(l) & 0x00FFFFFFu)))

/* The synthetic filesystems. Each supplies one of these and vfs.c mounts
 * it; neither has any state a caller can change. */
const vfs_ops_t *devfs_ops(void);
void devfs_init(void);

const vfs_ops_t *procfs_ops(void);
void procfs_init(void);
