/* kernel/fs/flock.h - M100: POSIX record locks.
 *
 * ---- who asked ----------------------------------------------------------
 *
 * sqlite. Its unix VFS takes an fcntl(F_SETLK) byte-range lock before
 * every read transaction and a second one before every write, and
 * treats any answer other than "granted" or "somebody else holds it" as
 * a disk I/O error - so on a kernel whose fcntl said EOPNOTSUPP, every
 * `INSERT` on this machine failed. <fcntl.h> had declared the three
 * lock commands and refused them since M89, on M65's rule: a lock that
 * always succeeds protects nothing while telling every caller it did.
 * That rule is why this file exists rather than a stub - the sixth
 * library of M100's stack is the first program here that needs the
 * locks to be real, and it is the caller that checks them.
 *
 * ---- what it is ---------------------------------------------------------
 *
 * The POSIX model, kept to what it says: locks belong to a PROCESS on an
 * INODE, cover a byte range [start, end) where "to end of file" is an
 * open end, are advisory (nothing here refuses a read or write because
 * of one), and every lock a process holds on a file is released when
 * that process closes ANY descriptor naming the file - which is the
 * clause everybody finds surprising and which sqlite's own source
 * comments on at length, so it is honoured exactly.
 *
 * Pure logic over a fixed table, with no scheduler in it: a request
 * that would block is reported as FLOCK_CONFLICT and the caller decides
 * whether to wait (F_SETLKW parks on FLOCK_CHAN in syscall.c and is
 * woken by every release). That split is what lets tests/test_flock.c
 * grade the range arithmetic - the splits, the merges, the open end -
 * on the host in microseconds, which is where a lock table's bugs are.
 *
 * Deadlock detection (EDEADLK) is not attempted. Two processes each
 * waiting for the other's lock will wait, and the honest answer is
 * that nothing ported here has asked for the detection - sqlite never
 * uses F_SETLKW at all.
 */
#pragma once

#include <stdint.h>

#include "os_fs.h" /* system_api/include/os_fs.h - os_flock_t, OS_FLOCK_* */

/* How many locks the whole machine can hold at once. sqlite uses at
 * most three per open database; 128 is forty databases, and a table
 * that fills is reported (FLOCK_FULL -> ENOLCK) rather than grown. */
#define FLOCK_MAX 128

/* The wait channel an F_SETLKW parks on and every release wakes. A
 * function's address is a constant both syscall.c and sched.c can name
 * without this file owning any scheduler state. */
#define FLOCK_CHAN ((const void *)flock_count)

#define FLOCK_CONFLICT (-2) /* somebody else holds an overlapping lock */
#define FLOCK_FULL     (-3) /* no free entry to record it in */

/* Is there a lock held by a pid OTHER than `pid` on inode `ino` that
 * conflicts with a `type` lock over [start, start+len)? len 0 means to
 * end of file. Returns 1 and describes the conflicting lock in `out`
 * (type, start, len, pid; whence is always SEEK_SET) if so; 0 and sets
 * out->type to OS_FLOCK_UNLCK if not. This is F_GETLK. */
int flock_test(uint32_t ino, int pid, int type, uint64_t start, uint64_t len,
               os_flock_t *out);

/* Apply a lock (or, for OS_FLOCK_UNLCK, an unlock) over the range for
 * `pid` on `ino`. Returns 0 when done, FLOCK_CONFLICT when another pid's
 * lock is in the way (and the table is unchanged), FLOCK_FULL when the
 * result would need more entries than are free (also unchanged). A
 * process's own locks never conflict with its request: a new lock
 * replaces whatever of its own was in the range, which is how a read
 * lock becomes a write lock. */
int flock_set(uint32_t ino, int pid, int type, uint64_t start, uint64_t len);

/* Every lock `pid` holds on `ino`. The POSIX close rule: called when a
 * process closes any descriptor for the file. Returns how many entries
 * were released, so a caller knows whether anybody might be waiting. */
int flock_release_file(uint32_t ino, int pid);

/* Every lock `pid` holds anywhere. Called when the process exits. */
int flock_release_pid(int pid);

/* Entries in use - for the tests and the exhaustion self-test. */
int flock_count(void);
