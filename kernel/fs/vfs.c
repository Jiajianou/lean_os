#include "vfs.h"

#include "drivers/klog.h"
#include "leanfs.h"

void vfs_init(void) {
    leanfs_init();
}

int64_t vfs_read(const char *name, void *buf, size_t maxlen) {
    return leanfs_read(name, buf, maxlen);
}

int vfs_write(const char *name, const void *buf, size_t len) {
    return leanfs_write(name, buf, len);
}

int vfs_exists(const char *name) {
    return leanfs_exists(name);
}

size_t vfs_list(char *buf, size_t maxlen) {
    return leanfs_list(buf, maxlen);
}

void vfs_sync(void) {
    /* See vfs.h. Nothing to write back: leanfs_write_file's own
     * ata_write_sectors calls are synchronous, so a file is on the
     * platter by the time vfs_write returns to its caller. */
    klog_puts("[vfs] sync: leanfs is write-through, nothing buffered to flush.\n");
}
