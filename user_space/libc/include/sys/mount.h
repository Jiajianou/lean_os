#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT     32
#define MS_MANDLOCK    64
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_BIND        4096
#define MS_MOVE        8192
#define MS_REC         16384
#define MS_SILENT      32768

#define MNT_FORCE      1
#define MNT_DETACH     2
#define MNT_EXPIRE     4

int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);

#ifdef __cplusplus
}
#endif
