/* user_space/libc/include/utime.h - M88
 *
 * The oldest spelling of "stamp this file with a time I choose", and the
 * one `install -p` and every tar-like unpack still call.
 *
 * leanfs stores one timestamp per inode, not three, so actime is
 * accepted and ignored. That is stated in the header rather than
 * discovered: a program that sets an access time here and reads it back
 * gets the modification time, and knowing that in advance is the
 * difference between a documented limit and a bug.
 */
#pragma once

#include <time.h>

struct utimbuf {
    time_t actime;  /* accepted and ignored - this filesystem stores one time */
    time_t modtime;
};

/* A NULL `times` means "now", which is the only case that needs no
 * privilege anywhere and is what `touch` on an existing file does. */
int utime(const char *path, const struct utimbuf *times);
