#pragma once

#include <stdint.h>

#include "os_fs.h"

#define FLOCK_MAX 128

#define FLOCK_CHAN ((const void *)flock_count)

#define FLOCK_CONFLICT (-2)
#define FLOCK_FULL     (-3)

int flock_test(uint32_t ino, int pid, int type, uint64_t start, uint64_t len,
               os_flock_t *out);

int flock_set(uint32_t ino, int pid, int type, uint64_t start, uint64_t len);

int flock_release_file(uint32_t ino, int pid);

int flock_release_pid(int pid);

int flock_count(void);
