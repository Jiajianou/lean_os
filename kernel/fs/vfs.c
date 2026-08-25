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

uint32_t vfs_free_blocks(void) {
    return leanfs_free_blocks();
}

int vfs_unlink(const char *path) {
    return leanfs_unlink(path);
}

int vfs_rename(const char *old_path, const char *new_path) {
    return leanfs_rename(old_path, new_path);
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

/* M59 */
int vfs_rmdir(const char *path) {
    return leanfs_rmdir(path);
}

int vfs_stat(const char *path, leanfs_stat_t *out) {
    return leanfs_stat(path, out);
}

int vfs_open(const char *path, int create) {
    return leanfs_open(path, create);
}

int64_t vfs_handle_read(int handle, void *buf, size_t len, uint32_t off) {
    return leanfs_handle_read(handle, buf, len, off);
}

int64_t vfs_handle_write(int handle, const void *buf, size_t len, uint32_t off) {
    return leanfs_handle_write(handle, buf, len, off);
}

uint32_t vfs_handle_size(int handle) {
    return leanfs_handle_size(handle);
}

int vfs_handle_truncate(int handle) {
    return leanfs_handle_truncate(handle);
}
