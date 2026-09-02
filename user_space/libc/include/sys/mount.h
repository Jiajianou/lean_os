/* user_space/libc/include/sys/mount.h - M89
 *
 * `mount` and `umount`, which this machine cannot do and says so.
 *
 * There IS a mount table here - M87 built one, and /dev and /proc are
 * mounted on it - but it is assembled by the kernel at boot and there is
 * no syscall that lets a program add to it. So both calls are truthful
 * failures with ENOSYS rather than stubs returning 0, for the reason M65
 * set out at length: a program told its mount succeeded would then
 * believe a filesystem is there.
 *
 * The MS_* flags are defined because code that never calls mount still
 * has to compile when it mentions them in a table of options, and
 * because a flag with no call behind it cannot mislead anyone.
 */
#pragma once

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

/* umount2()'s flags. Same status as the above. */
#define MNT_FORCE      1
#define MNT_DETACH     2
#define MNT_EXPIRE     4

int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);
