/* user_space/libc/include/sys/random.h - M100
 *
 * getrandom(2), which glibc has had since 2.25 and which mbedtls's
 * entropy_poll.c asks for by name under Linux. Over SYS_getrandom, and
 * see that entry for what the flags do here (nothing) and why this
 * never blocks or returns short.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

ssize_t getrandom(void *buf, size_t len, unsigned int flags);

/* getentropy(3): up to 256 bytes, all or nothing, 0 or -1. The BSD
 * spelling, which glibc also has and which toybox's portability layer
 * reaches for the moment it sees this header exist - its build broke
 * the day <sys/random.h> appeared here without it, which is the header
 * rule again from the other side: a header that exists promises
 * everything its namesake has. */
int getentropy(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
