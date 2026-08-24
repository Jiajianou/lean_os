#include "vfs.h"

#include "drivers/klog.h"
#include "leanfs.h"

void vfs_init(void) {
    leanfs_init();
}

int64_t vfs_read(const char *path, void *buf, size_t maxlen) {
    return leanfs_read(path, buf, maxlen);
}

int vfs_write(const char *path, const void *buf, size_t len) {
    return leanfs_write(path, buf, len);
}

int vfs_exists(const char *path) {
    return leanfs_exists(path);
}

int vfs_is_dir(const char *path) {
    return leanfs_is_dir(path);
}

int vfs_mkdir(const char *path) {
    return leanfs_mkdir(path);
}

size_t vfs_list(const char *path, char *buf, size_t maxlen) {
    return leanfs_list(path, buf, maxlen);
}

void vfs_sync(void) {
    /* See vfs.h. Nothing to write back: leanfs_write_file's own
     * ata_write_sectors calls are synchronous, so a file is on the
     * platter by the time vfs_write returns to its caller. */
    klog_puts("[vfs] sync: leanfs is write-through, nothing buffered to flush.\n");
}
