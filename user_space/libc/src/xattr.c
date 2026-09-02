/* user_space/libc/src/xattr.c - M89
 *
 * Twelve refusals. See <sys/xattr.h> for why ENOTSUP rather than an
 * empty success, which is the only interesting decision in this file.
 */
#include <sys/xattr.h>

#include <errno.h>

static ssize_t refuse_ssize(void) {
    errno = ENOTSUP;
    return -1;
}

static int refuse_int(void) {
    errno = ENOTSUP;
    return -1;
}

ssize_t getxattr(const char *path, const char *name, void *value, size_t size) {
    (void)path; (void)name; (void)value; (void)size;
    return refuse_ssize();
}

ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) {
    (void)path; (void)name; (void)value; (void)size;
    return refuse_ssize();
}

ssize_t fgetxattr(int fd, const char *name, void *value, size_t size) {
    (void)fd; (void)name; (void)value; (void)size;
    return refuse_ssize();
}

ssize_t listxattr(const char *path, char *list, size_t size) {
    (void)path; (void)list; (void)size;
    return refuse_ssize();
}

ssize_t llistxattr(const char *path, char *list, size_t size) {
    (void)path; (void)list; (void)size;
    return refuse_ssize();
}

ssize_t flistxattr(int fd, char *list, size_t size) {
    (void)fd; (void)list; (void)size;
    return refuse_ssize();
}

int setxattr(const char *path, const char *name, const void *value, size_t size,
             int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags;
    return refuse_int();
}

int lsetxattr(const char *path, const char *name, const void *value,
              size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags;
    return refuse_int();
}

int fsetxattr(int fd, const char *name, const void *value, size_t size,
              int flags) {
    (void)fd; (void)name; (void)value; (void)size; (void)flags;
    return refuse_int();
}

int removexattr(const char *path, const char *name) {
    (void)path; (void)name;
    return refuse_int();
}

int lremovexattr(const char *path, const char *name) {
    (void)path; (void)name;
    return refuse_int();
}

int fremovexattr(int fd, const char *name) {
    (void)fd; (void)name;
    return refuse_int();
}
