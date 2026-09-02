/* user_space/libc/include/sys/xattr.h - M89
 *
 * Extended attributes, which leanfs does not have.
 *
 * **Every call here is a refusal, and that is the content of this
 * header.** An extended attribute is a named byte string attached to a
 * file, stored by the filesystem. leanfs's inode is 64 bytes with no
 * room for one and no indirection to put one behind - see
 * kernel/fs/leanfs_format.h - so there is nowhere for a value to go.
 *
 * The getters report ENOTSUP, which is precisely what a filesystem
 * without xattr support returns on Linux and exactly what every caller
 * already handles: `cp -a` and `tar` both probe with a getter, see the
 * error, and copy the file without attributes. A getter that returned 0
 * bytes would instead say "this file has no attributes", which is a
 * different claim and one that would make `tar` silently drop them on a
 * filesystem that did have some.
 *
 * The header exists rather than the calls being absent because
 * portability code includes it unconditionally on any system that is not
 * one of the three BSDs - so the choice is between this and a patch to
 * somebody else's #if.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

#define XATTR_CREATE  1
#define XATTR_REPLACE 2

ssize_t getxattr(const char *path, const char *name, void *value, size_t size);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size);
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size);

ssize_t listxattr(const char *path, char *list, size_t size);
ssize_t llistxattr(const char *path, char *list, size_t size);
ssize_t flistxattr(int fd, char *list, size_t size);

int setxattr(const char *path, const char *name, const void *value, size_t size,
             int flags);
int lsetxattr(const char *path, const char *name, const void *value,
              size_t size, int flags);
int fsetxattr(int fd, const char *name, const void *value, size_t size,
              int flags);

int removexattr(const char *path, const char *name);
int lremovexattr(const char *path, const char *name);
int fremovexattr(int fd, const char *name);

#ifdef __cplusplus
}
#endif
