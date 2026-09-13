#include "vfsops.h"

#include "arch/x86_64/tsc.h"
#include "dev/pty.h"
#include "dev/random.h"
#include "dev/tty.h"
#include "drivers/klog.h"
#include "lib/libk.h"

enum {
    DEV_DIR = 0,
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

#define DEV_PTS_DIR  0x100
#define DEV_PTS_BASE 0x200
#define DEV_PTM_BASE 0x300

#define DEV_IS_PTS(c) ((c) >= DEV_PTS_BASE && (c) < DEV_PTS_BASE + PTY_MAX)
#define DEV_IS_PTM(c) ((c) >= DEV_PTM_BASE && (c) < DEV_PTM_BASE + PTY_MAX)

void devfs_init(void) {
}

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
                return -1;
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

#define DEVFS_INO_BASE 0x40000000u

static int dev_stat(const char *rel, leanfs_stat_t *out) {
    int d = lookup(rel);
    if (d < 0) {
        return -1;
    }
    out->size = 0;
    out->mtime = 0;
    out->is_dir = (d == DEV_DIR || d == DEV_PTS_DIR) ? 1 : 0;
    out->is_link = 0;
    out->inode = DEVFS_INO_BASE + (uint32_t)d;
    return 0;
}

static int dev_open(const char *rel, int create) {
    (void)create;
    int d = lookup(rel);
    if (d < 0 || d == DEV_DIR || d == DEV_PTS_DIR) {
        return -1;
    }
    if (d == DEV_PTMX) {
        int n = pty_alloc();
        if (n < 0) {
            return -1;
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
    out->inode = DEVFS_INO_BASE + (uint32_t)handle;
    return 0;
}

static int64_t dev_read(int handle, void *buf, size_t len, uint32_t off) {
    (void)off;
    uint8_t *b = (uint8_t *)buf;
    if (DEV_IS_PTM(handle)) {
        return pty_master_read(handle - DEV_PTM_BASE, (char *)b, (uint32_t)len);
    }
    if (DEV_IS_PTS(handle)) {
        return pty_slave_read(handle - DEV_PTS_BASE, (char *)b, (uint32_t)len);
    }
    switch (handle) {
    case DEV_NULL:
        return 0;
    case DEV_ZERO:
    case DEV_FULL:
        k_memset(b, 0, len);
        return (int64_t)len;
    case DEV_RANDOM:
    case DEV_URANDOM:
        random_bytes(b, len);
        return (int64_t)len;
    case DEV_TTY: {
        uint32_t n = tty_read(tty_console(), (char *)b, (uint32_t)len);
        return (int64_t)n;
    }
    case DEV_CONSOLE:
        return 0;
    default:
        return -1;
    }
}

static int64_t dev_write(int handle, const void *buf, size_t len, uint32_t off) {
    (void)off;
    const char *b = (const char *)buf;
    if (DEV_IS_PTM(handle)) {
        return pty_master_write(handle - DEV_PTM_BASE, b, (uint32_t)len);
    }
    if (DEV_IS_PTS(handle)) {
        return pty_slave_write(handle - DEV_PTS_BASE, b, (uint32_t)len);
    }
    switch (handle) {
    case DEV_NULL:
    case DEV_ZERO:
        return (int64_t)len;
    case DEV_FULL:
        return -1;
    case DEV_RANDOM:
    case DEV_URANDOM:
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

static void dev_close(int handle) {
    if (DEV_IS_PTM(handle)) {
        pty_master_closed(handle - DEV_PTM_BASE);
    } else if (DEV_IS_PTS(handle)) {
        pty_slave_closed(handle - DEV_PTS_BASE);
    }
}

static int dev_readable(int handle) {
    if (DEV_IS_PTM(handle)) {
        return pty_master_readable(handle - DEV_PTM_BASE);
    }
    if (DEV_IS_PTS(handle)) {
        return pty_slave_readable(handle - DEV_PTS_BASE);
    }
    if (handle == DEV_TTY) {
        return tty_readable(tty_console()) > 0 ? 1 : 0;
    }
    return 1;
}

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
    uint32_t i = *cookie;
    if (i == 0) {
        i = 1;
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
    out->is_link = 0;
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
