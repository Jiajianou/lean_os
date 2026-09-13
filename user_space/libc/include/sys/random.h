#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

ssize_t getrandom(void *buf, size_t len, unsigned int flags);

int getentropy(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
