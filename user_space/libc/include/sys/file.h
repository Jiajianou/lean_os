#pragma once

#include <fcntl.h>

/* M160. flock(2)'s operations. The numbers are 4.2BSD's, which Linux, the
   BSDs and macOS all kept, and which anything calling flock has compiled
   against for forty years. */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#ifdef __cplusplus
extern "C" {
#endif

int flock(int descriptor, int operation);

#ifdef __cplusplus
}
#endif
