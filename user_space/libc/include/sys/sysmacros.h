#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The encoding is Linux's, and that is a contract rather than a preference:
   a dev_t crosses between this libc and programs that were written against
   that layout, and st_rdev is a number they take apart themselves. */
#define major(device)      ((unsigned int)(((device) >> 8) & 0xFFFu) | \
                            (unsigned int)(((device) >> 32) & ~0xFFFu))
#define minor(device)      ((unsigned int)((device) & 0xFFu) | \
                            (unsigned int)(((device) >> 12) & ~0xFFu))
#define makedev(high, low) ((dev_t)(((dev_t)((high) & 0xFFFu) << 8) |       \
                                    ((dev_t)((high) & ~0xFFFu) << 32) |     \
                                    ((dev_t)((low) & 0xFFu)) |              \
                                    ((dev_t)((low) & ~0xFFu) << 12)))

#ifdef __cplusplus
}
#endif
