/* user_space/libc/include/sys/param.h - M98
 *
 * The BSD grab-bag header, provided because ported code includes it by
 * reflex. Nothing here is new information: MAXPATHLEN restates
 * <limits.h>'s PATH_MAX, MIN/MAX restate two ternaries, and the bit
 * arithmetic restates <stdint.h>. The header exists because a build
 * that says `#include <sys/param.h>` does not care that its contents
 * were available under other names - to a build, a missing header is a
 * missing feature (M94's rule; libctf named this one first).
 *
 * Deliberately not here: NODEV, MAXSYMLINKS, the scheduling constants,
 * HZ. Each would be a number invented to fill a slot, and a caller that
 * actually reads one deserves the compile error that makes it say so.
 */
#pragma once

#include <endian.h>
#include <limits.h>
#include <sys/types.h>

#define MAXPATHLEN PATH_MAX

/* Bits per byte. Spelled as a name because bitmap code indexes with it. */
#define NBBY 8

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/* How many y-sized units cover x, and x rounded up to a multiple of y.
 * The classic forms, kept classic: callers pass y > 0. */
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x)   ((((x) - 1) & (x)) == 0)
