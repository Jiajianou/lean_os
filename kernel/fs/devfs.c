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
    DEV_COUNT
};

static const char *const DEV_NAMES[DEV_COUNT] = {
    "", "null", "zero", "full", "random", "urandom", "tty", "console",
};

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
static uint64_t rng_state;

static uint64_t next_random(void) {
    /* Re-mixed with the TSC on every call, so two reads separated by any
     * real work do not continue one predictable sequence. Not entropy -
     * a counter an attacker can also read - but it costs one instruction
     * and makes the common "seed something at startup" case depend on
     * when the program ran rather than only on the boot. */
    rng_state ^= tsc_read();
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ULL;
}

void devfs_init(void) {
    rng_state = tsc_read() | 1u; /* never zero - xorshift is stuck there */
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
    return -1;
}

static int dev_exists(const char *rel) {
    return lookup(rel) >= 0;
}

static int dev_is_dir(const char *rel) {
    return lookup(rel) == DEV_DIR;
}

static int dev_stat(const char *rel, leanfs_stat_t *out) {
    int d = lookup(rel);
    if (d < 0) {
        return -1;
    }
    out->size = 0; /* a device has no length - see dev_size */
    out->mtime = 0;
    out->is_dir = (d == DEV_DIR) ? 1 : 0;
    return 0;
}

static int dev_open(const char *rel, int create) {
    (void)create; /* nothing here can be created, and asking is not an error */
    int d = lookup(rel);
    if (d < 0 || d == DEV_DIR) {
        return -1;
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
    if (handle <= 0 || handle >= DEV_COUNT) {
        return -1;
    }
    out->size = 0;
    out->mtime = 0;
    out->is_dir = 0;
    return 0;
}

static int64_t dev_read(int handle, void *buf, size_t len, uint32_t off) {
    (void)off; /* a device is not seekable; every read is "now" */
    uint8_t *b = (uint8_t *)buf;
    switch (handle) {
    case DEV_NULL:
        return 0; /* end of file, immediately and always */
    case DEV_ZERO:
    case DEV_FULL:
        k_memset(b, 0, len);
        return (int64_t)len;
    case DEV_RANDOM:
    case DEV_URANDOM: {
        size_t n = 0;
        while (n < len) {
            uint64_t r = next_random();
            for (int i = 0; i < 8 && n < len; i++) {
                b[n++] = (uint8_t)(r >> (i * 8));
            }
        }
        return (int64_t)len;
    }
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
        /* Writing to /dev/random adds entropy on a real system. There is
         * no pool here, so the bytes are mixed into the generator and
         * that is honestly all that happens. */
        for (size_t i = 0; i < len; i++) {
            rng_state ^= (uint64_t)(uint8_t)b[i] << ((i % 8) * 8);
        }
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

static int dev_readdir(const char *rel, uint32_t *cookie, leanfs_dir_entry_t *out) {
    if (lookup(rel) != DEV_DIR) {
        return -1;
    }
    /* The cookie is the next index, which is the simplest thing that can
     * be resumed - and unlike leanfs's byte offset it cannot go stale,
     * because this directory never changes. */
    uint32_t i = *cookie;
    if (i == 0) {
        i = 1; /* index 0 is the directory itself, not an entry in it */
    }
    if (i >= DEV_COUNT) {
        return 0;
    }
    out->inode = i;
    out->is_dir = 0;
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
};

const vfs_ops_t *devfs_ops(void) {
    return &DEVFS_OPS;
}
