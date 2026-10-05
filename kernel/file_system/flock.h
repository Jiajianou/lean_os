#pragma once

#include <stdint.h>

#include "os_file_system.h"

#define FLOCK_MAX 128

#define FLOCK_CHAN ((const void *)flock_count)

#define FLOCK_CONFLICT (-2)
#define FLOCK_FULL     (-3)

/* `pid` is the lock's OWNER, and POSIX makes that the process: every caller
   passes scheduler_record_lock_owner(task) - the thread-group id, what
   getpid() answers - never a thread's own id. Two owners conflict; one
   owner's ranges merge and split however many of its threads ask. */
int flock_test(uint32_t ino, int pid, int type, uint64_t start, uint64_t length,
               os_flock_t *out);

int flock_set(uint32_t ino, int pid, int type, uint64_t start, uint64_t length);

int flock_release_file(uint32_t ino, int pid);

int flock_release_pid(int pid);

int flock_count(void);
