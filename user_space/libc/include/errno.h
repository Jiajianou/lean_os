/* user_space/libc/include/errno.h - M80 groundwork
 *
 * `errno`, and an honest account of what it can and cannot tell you
 * here.
 *
 * This kernel's syscalls return -1 for almost every kind of failure and
 * do not carry a reason: SYS_open returns -1 for a path that does not
 * exist, for a path that is a directory, and for a descriptor table that
 * is full, and there is nowhere in its ABI for it to say which. So an
 * errno set from those returns would be a guess dressed as a diagnosis.
 *
 * What is here is therefore deliberately modest, and the modesty is the
 * point:
 *
 *  - `errno` is a real, writable variable, because that is what a C
 *    program written elsewhere expects to find and half of them set it
 *    themselves.
 *  - the E* numbers match Linux's, so a program comparing against
 *    ENOENT means here what it meant there.
 *  - libc sets it only where this system genuinely knows the reason,
 *    which today is the handful of places a wrapper checks its own
 *    arguments before making a syscall at all.
 *
 * The alternative - inventing ENOENT for every failed open - would be
 * exactly the kind of plausible fiction this project declined to write
 * for uids (M65) and for st_mode (M77). When a syscall here grows a real
 * error code, this is where it surfaces.
 */
#pragma once

extern int errno;

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EMLINK  31
#define EPIPE   32
#define EDOM    33
#define ERANGE  34
#define ENAMETOOLONG 36
#define ENOSYS  38
#define ENOTEMPTY 39
#define ELOOP   40
/* M88: a byte sequence that is not valid in this locale's encoding.
 * Unlike the socket codes below, this one is set: every conversion in
 * <wchar.h> reports a malformed or overlong UTF-8 sequence with it, and
 * a program that reads a file of unknown bytes will see it. */
#define EILSEQ  84
/* The socket and blocking-operation codes. Present because a program
 * that compares errno against them has to compile; nothing on this
 * machine sets them yet, for the reason at the top of this file. */
#define ENOTSOCK 88
#define EOPNOTSUPP 95
#define EADDRINUSE 98
#define ECONNREFUSED 111
#define ETIMEDOUT 110
#define EINPROGRESS 115
#define EALREADY 114
#define ECONNRESET 104
#define ECONNABORTED 103
/* M89: the rest of the socket errors, at Linux's numbers like the others
 * above. These ones ARE set: <sys/socket.h>'s refusals report through
 * them - a family this stack does not have, an option it cannot set, a
 * peer it cannot name. */
#define ENOTCONN        107
#define EAFNOSUPPORT    97
#define ESOCKTNOSUPPORT 94
#define EPROTONOSUPPORT 93
#define ENOPROTOOPT     92
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define ENETUNREACH     101
#define ENETDOWN        100
#define ENOTSUP EOPNOTSUPP
#define EHOSTUNREACH 113
#define ENETUNREACH 101
#define EWOULDBLOCK EAGAIN
