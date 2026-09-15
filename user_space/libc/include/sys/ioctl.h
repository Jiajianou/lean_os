#pragma once

#include "termios.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many bytes can be read from this descriptor without blocking. The
   kernel already has to know for poll; this is the same answer as a count
   rather than a readiness bit. */
#define FIONREAD 0x541B

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
