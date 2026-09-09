/* kernel/fs/devfs.c - M87
 *
 * `/dev`, and the handful of files in it that a program written
 * elsewhere expects to find.
 *
 * This is the most common thing a ported program touches that this
 * machine could not answer. `/dev/null` is not a file with no bytes in
 * it - it is a rule that reads return end-of-file and writes are
 * accepted and discarded. leanfs cannot express that without learning to
 * lie about what a file is, which is why M87 builds a seam instead of
 * adding a special case.
 *
 * What is here is what something asks for, in M63's tradition: null,
 * zero, full, random/urandom, tty and console. Not a device node for the
 * disk, not a device node for the framebuffer - those exist behind
 * syscalls with capability gates (M65), and giving them a path would be
 * giving them a second way in that the gate does not cover.
 */
#include "vfsops.h"

#include "arch/x86_64/tsc.h"
#include "dev/pty.h"
#include "dev/random.h" /* M100 */
#include "dev/tty.h"
#include "drivers/klog.h"
#include "lib/libk.h"

/* The handle IS the index into this table, which is what lets every
 * operation below be a switch rather than a lookup. */
enum {
    DEV_DIR = 0, /* the directory itself */
    DEV_NULL,
    DEV_ZERO,
    DEV_FULL,
    DEV_RANDOM,
    DEV_URANDOM,
    DEV_TTY,
    DEV_CONSOLE,
    DEV_PTMX,
    DEV_COUNT
};

static const char *const DEV_NAMES[DEV_COUNT] = {
    "", "null", "zero", "full", "random", "urandom", "tty", "console", "ptmx",
};

/* ---- M85: the pty numbers, above the fixed table ----------------------
 *
 * `/dev/pts` is a directory whose contents change, which is the first
 * thing under /dev that is not one of a fixed list - so it gets codes of
 * its own above DEV_COUNT rather than a row in the table. The three
 * bases are far enough apart to read at a glance in a log, which is
 * worth more here than density: every one of these numbers ends up in a
 * file descriptor and in an inode number.
 *
 * DEV_PTM_BASE is never returned by lookup(). A master has no path -
 * opening /dev/ptmx *creates* one, and there is no name that names an
 * existing master. That asymmetry is real and is why /dev/ptmx exists at
 * all rather than /dev/ptm/<n>. */
#define DEV_PTS_DIR  0x100
#define DEV_PTS_BASE 0x200 /* + n: the slave, /dev/pts/<n> */
#define DEV_PTM_BASE 0x300 /* + n: the master, from opening /dev/ptmx */

#define DEV_IS_PTS(c) ((c) >= DEV_PTS_BASE && (c) < DEV_PTS_BASE + PTY_MAX)
#define DEV_IS_PTM(c) ((c) >= DEV_PTM_BASE && (c) < DEV_PTM_BASE + PTY_MAX)

/* ---- the number generator, and exactly what it is not -----------------
 *
 * xorshift64*, seeded from the timestamp counter. It is fast, it has a
 * long period, and it is NOT cryptographic - a caller who can observe
 * two outputs can predict every one after them.
 *
 * That matters because `/dev/random` traditionally promises otherwise,
 * and a program that asks for it is often asking for exactly the
 * property this cannot provide. It is provided anyway, because the
 * alternative is that the path does not exist and the program fails at
 * startup rather than at the one operation that needed real entropy -
 * and because the overwhelming majority of reads from it on any system
 * are a hash seed or a temporary filename. The honest thing available is
 * to say so here and to make `/dev/random` and `/dev/urandom` the same
 * device, which is what they are: there is no entropy pool to block on,
 * so a blocking variant would be a lie of a different shape.
 */
/* M100: the xorshift that stood here for ten milestones is gone, and
 * the paragraph above it is kept because it was true and is the reason
 * kernel/dev/random.c exists: a TLS library asked /dev/urandom for a
 * key. Both names now read the generator described there - a ChaCha20
 * stream under a key every interrupt of the boot has been mixed into
 * and that is replaced the moment it produces output - and they are
 * still one device, because there is still no entropy estimate to
 * block on and inventing one would be the lie of the other shape. */
void devfs_init(void) {
}

/* "/null" -> DEV_NULL. "/" is the directory. -1 for anything else. */
static int lookup(const char *rel) {
    if (!rel || rel[0] != '/') {
        return -1;
    }
    if (rel[1] == '\0') {
        return DEV_DIR;
    }
    for (int i = 1; i < DEV_COUNT; i++) {
        if (k_strcmp(rel + 1, DEV_NAMES[i]) == 0) {
            return i;
        }
    }
    /* M85: /dev/pts, and the numbers in it.
     *
     * A slave that names a pair nobody allocated does not exist, rather
     * than existing and failing to open - which is what makes
     * `ls /dev/pts` show the terminals that are actually running and
     * makes a stale path in a saved session fail at the open with
     * "no such file". */
    if (k_strcmp(rel + 1, "pts") == 0) {
        return DEV_PTS_DIR;
    }
    if (rel[1] == 'p' && rel[2] == 't' && rel[3] == 's' && rel[4] == '/') {
        const char *d = rel + 5;
        if (*d == '\0') {
            return -1;
        }
        int n = 0;
        for (; *d; d++) {
            if (*d < '0' || *d > '9') {
                return -1; /* /dev/pts/x is not a terminal, it is a typo */
            }
            n = n * 10 + (*d - '0');
            if (n >= PTY_MAX) {
                return -1;
            }
        }
        return pty_valid(n) ? DEV_PTS_BASE + n : -1;
    }
    return -1;
}

static int dev_exists(const char *rel) {
    return lookup(rel) >= 0;
}

static int dev_is_dir(const char *rel) {
    int c = lookup(rel);
    return (c == DEV_DIR || c == DEV_PTS_DIR);
}

/* ---- M89: inode numbers, and why these start where they do ------------
 *
 * A program deciding whether two paths are the same file compares
 * (st_dev, st_ino). st_dev is 0 everywhere here - there is one device
 * namespace and nothing to number it with - so st_ino has to carry the
 * whole answer, which means /dev/null and leanfs inode 1 must not share
 * a number.
 *
 * leanfs's inodes run from 0 to LEANFS_MAX_INODES (131072), so the two
 * synthetic filesystems number from bases well above that and well apart
 * from each other. It is a partition of one number space rather than a
 * device field, which is the cheaper of the two and is honest as long as
 * it is written down - so it is written down here, in procfs.c, and in
 * os_stat_t's own note.
 */
#define DEVFS_INO_BASE 0x40000000u

static int dev_stat(const char *rel, leanfs_stat_t *out) {
    int d = lookup(rel);
    if (d < 0) {
        return -1;
    }
    out->size = 0; /* a device has no length - see dev_size */
    out->mtime = 0;
    out->is_dir = (d == DEV_DIR || d == DEV_PTS_DIR) ? 1 : 0;
    out->is_link = 0;
    out->inode = DEVFS_INO_BASE + (uint32_t)d; /* M89 - see DEVFS_INO_BASE */
    return 0;
}

static int dev_open(const char *rel, int create) {
    (void)create; /* nothing here can be created, and asking is not an error */
    int d = lookup(rel);
    if (d < 0 || d == DEV_DIR || d == DEV_PTS_DIR) {
        return -1;
    }
    /* M85: the one path on this machine where opening a file has a side
     * effect. /dev/ptmx is not a device you read - it is a *request for a
     * terminal*, and every open of it produces a different one. That is
     * unusual enough to say out loud; it is also exactly what every Unix
     * does, and the reason ptsname() exists to ask which one you got. */
    if (d == DEV_PTMX) {
        int n = pty_alloc();
        if (n < 0) {
            return -1; /* all eight in use - see pty.h */
        }
        return DEV_PTM_BASE + n;
    }
    if (DEV_IS_PTS(d)) {
        pty_slave_opened(d - DEV_PTS_BASE);
    }
    return d;
}

static uint32_t dev_size(int handle) {
    (void)handle;
    /* Zero, for every one of them, and it is the truthful answer rather
     * than a placeholder: none of these has a length. A program that
     * stats /dev/zero to find out how much it can read is asking a
     * question the file does not have - which is why `cat /dev/zero`
     * runs forever everywhere rather than copying some number of bytes. */
    return 0;
}

static int dev_handle_stat(int handle, leanfs_stat_t *out) {
    int fixed = (handle > 0 && handle < DEV_COUNT);
    if (!fixed && !DEV_IS_PTS(handle) && !DEV_IS_PTM(handle)) {
        return -1;
    }
    out->size = 0;
    out->mtime = 0;
    out->is_dir = 0;
    out->is_link = 0;
    out->inode = DEVFS_INO_BASE + (uint32_t)handle; /* M89 */
    return 0;
}

static int64_t dev_read(int handle, void *buf, size_t len, uint32_t off) {
    (void)off; /* a device is not seekable; every read is "now" */
    uint8_t *b = (uint8_t *)buf;
    if (DEV_IS_PTM(handle)) {
        return pty_master_read(handle - DEV_PTM_BASE, (char *)b, (uint32_t)len);
    }
    if (DEV_IS_PTS(handle)) {
        return pty_slave_read(handle - DEV_PTS_BASE, (char *)b, (uint32_t)len);
    }
    switch (handle) {
    case DEV_NULL:
        return 0; /* end of file, immediately and always */
    case DEV_ZERO:
    case DEV_FULL:
        k_memset(b, 0, len);
        return (int64_t)len;
    case DEV_RANDOM:
    case DEV_URANDOM:
        random_bytes(b, len); /* M100 - see kernel/dev/random.h */
        return (int64_t)len;
    case DEV_TTY: {
        uint32_t n = tty_read(tty_console(), (char *)b, (uint32_t)len);
        return (int64_t)n;
    }
    case DEV_CONSOLE:
        return 0; /* write-only in practice; reading the console is what /dev/tty is for */
    default:
        return -1;
    }
}

static int64_t dev_write(int handle, const void *buf, size_t len, uint32_t off) {
    (void)off;
    const char *b = (const char *)buf;
    /* The two halves of a pty, and they are not symmetrical. A write to
     * the master is a *keystroke* and goes through the line discipline;
     * a write to the slave is a program's output and goes through output
     * processing. Getting these the same way round is the whole of the
     * device. */
    if (DEV_IS_PTM(handle)) {
        return pty_master_write(handle - DEV_PTM_BASE, b, (uint32_t)len);
    }
    if (DEV_IS_PTS(handle)) {
        return pty_slave_write(handle - DEV_PTS_BASE, b, (uint32_t)len);
    }
    switch (handle) {
    case DEV_NULL:
    case DEV_ZERO:
        return (int64_t)len; /* accepted and discarded, which is the whole point */
    case DEV_FULL:
        /* The one device whose purpose is to fail. A program testing how
         * it handles a full disk needs somewhere that is always full,
         * and it is worth having precisely because nothing else here
         * refuses a write. */
        return -1;
    case DEV_RANDOM:
    case DEV_URANDOM:
        /* Writing to /dev/random adds entropy on a real system, and
         * M100 made that true here: the bytes are fed to the pool the
         * way an interrupt's timestamp is, and nothing is credited for
         * them, because nothing is credited for anything (random.h). */
        random_feed(b, len);
        return (int64_t)len;
    case DEV_TTY:
    case DEV_CONSOLE:
        for (size_t i = 0; i < len; i++) {
            klog_putc(b[i]);
        }
        return (int64_t)len;
    default:
        return -1;
    }
}

/* M85: the last thing holding this handle has gone.
 *
 * This is why vfs_ops_t grew a close hook in M101, and a pty is the
 * second filesystem object here to need one: the fixed table has eight
 * rows and a terminal emulator that exits without giving one back leaks
 * it for the life of the boot. */
static void dev_close(int handle) {
    if (DEV_IS_PTM(handle)) {
        pty_master_closed(handle - DEV_PTM_BASE);
    } else if (DEV_IS_PTS(handle)) {
        pty_slave_closed(handle - DEV_PTS_BASE);
    }
}

/* M85: would a read on this handle return without waiting?
 *
 * Every other device under /dev answers "yes, always" and means it -
 * /dev/zero has infinite bytes and /dev/null has none, and neither is a
 * reason to block. A pty is the first thing here that is genuinely
 * sometimes empty, which is what this hook exists for. */
static int dev_readable(int handle) {
    if (DEV_IS_PTM(handle)) {
        return pty_master_readable(handle - DEV_PTM_BASE);
    }
    if (DEV_IS_PTS(handle)) {
        return pty_slave_readable(handle - DEV_PTS_BASE);
    }
    if (handle == DEV_TTY) {
        /* /dev/tty is the console, and the console's input arrives from
         * a keyboard interrupt. Same question, same answer shape. */
        return tty_readable(tty_console()) > 0 ? 1 : 0;
    }
    return 1;
}

/* M85: which terminal does this handle name? See vfs_ops_t.tty_of.
 *
 * /dev/tty and /dev/console are the console's two names, and both answer
 * with it - which is what makes `stty` work on either. A master and its
 * slave answer with the *same* terminal, because they are two ends of
 * one: setting the window size on the master is how a terminal emulator
 * tells the program in it that the window changed, and a pair with two
 * termios structs would be two terminals wearing one name. */
static struct tty *dev_tty_of(int handle, int *pty_number) {
    if (DEV_IS_PTM(handle)) {
        *pty_number = handle - DEV_PTM_BASE;
        return (struct tty *)pty_tty(*pty_number);
    }
    if (DEV_IS_PTS(handle)) {
        *pty_number = handle - DEV_PTS_BASE;
        return (struct tty *)pty_tty(*pty_number);
    }
    if (handle == DEV_TTY || handle == DEV_CONSOLE) {
        *pty_number = -1;
        return (struct tty *)tty_console();
    }
    return (struct tty *)0;
}

static int dev_readdir(const char *rel, uint32_t *cookie, leanfs_dir_entry_t *out) {
    int c = lookup(rel);
    /* M85: /dev/pts lists the terminals that exist right now, which makes
     * it the first directory on this machine whose contents change
     * between two readdir calls. The cookie is still an index and still
     * cannot go stale in the way leanfs's byte offset can - a pair freed
     * mid-walk is skipped rather than misread. */
    if (c == DEV_PTS_DIR) {
        for (uint32_t n = *cookie; n < PTY_MAX; n++) {
            if (!pty_valid((int)n)) {
                continue;
            }
            out->inode = DEV_PTS_BASE + n;
            out->is_dir = 0;
            out->is_link = 0;
            out->name[0] = (char)('0' + (n % 10));
            out->name[1] = '\0';
            *cookie = n + 1;
            return 1;
        }
        return 0;
    }
    if (c != DEV_DIR) {
        return -1;
    }
    /* The cookie is the next index, which is the simplest thing that can
     * be resumed. Index DEV_COUNT is `pts`, which has no row in the
     * fixed table because it is a directory rather than a device. */
    uint32_t i = *cookie;
    if (i == 0) {
        i = 1; /* index 0 is the directory itself, not an entry in it */
    }
    if (i == DEV_COUNT) {
        out->inode = DEV_PTS_DIR;
        out->is_dir = 1;
        out->is_link = 0;
        k_strlcpy(out->name, "pts", sizeof(out->name));
        *cookie = i + 1;
        return 1;
    }
    if (i > DEV_COUNT) {
        return 0;
    }
    out->inode = i;
    out->is_dir = 0;
    out->is_link = 0; /* M89: no links under /dev - see leanfs_dir_entry_t */
    k_strlcpy(out->name, DEV_NAMES[i], sizeof(out->name));
    *cookie = i + 1;
    return 1;
}

static const vfs_ops_t DEVFS_OPS = {
    .stat = dev_stat,
    .is_dir = dev_is_dir,
    .exists = dev_exists,
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
    .size = dev_size,
    .handle_stat = dev_handle_stat,
    .readdir = dev_readdir,
    .close = dev_close,
    .readable = dev_readable,
    .tty_of = dev_tty_of,
};

const vfs_ops_t *devfs_ops(void) {
    return &DEVFS_OPS;
}
