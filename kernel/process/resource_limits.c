#include "resource_limits.h"

#include "scheduler/scheduler.h"
#include "process.h"

int resource_limit_for(uint64_t resource, os_rlimit_t *out) {
    if (!out) {
        return -1;
    }
    switch (resource) {
    case OS_RLIMIT_NOFILE:
        out->current = out->maximum = MAX_FILE_DESCRIPTORS;
        return 0;
    case OS_RLIMIT_NPROC:
        out->current = out->maximum = MAX_TASKS;
        return 0;
    case OS_RLIMIT_STACK:
        out->current = out->maximum = OS_MAIN_STACK_MAX_BYTES;
        return 0;
    /* No core dumps here, and no niceness to lower: this scheduler has two
       priority CLASSES rather than a range, so zero is the truth rather than
       a default. base/posix/can_lower_nice_to.cc reads the second one and
       correctly concludes no. */
    case OS_RLIMIT_CORE:
    case OS_RLIMIT_NICE:
    case OS_RLIMIT_RTPRIO:
        out->current = out->maximum = 0;
        return 0;
    /* Unbounded, and each of these is unbounded for a reason rather than by
       omission: nothing here limits how long a process may run, how large a
       file may be beyond what leanfs can address, how far the break may move
       beyond the address space it is in, or how much may be locked on a
       machine that never evicts. */
    case OS_RLIMIT_CPU:
    case OS_RLIMIT_FSIZE:
    case OS_RLIMIT_DATA:
    case OS_RLIMIT_AS:
    case OS_RLIMIT_MEMLOCK:
    case OS_RLIMIT_RSS:
    case OS_RLIMIT_SIGPENDING:
    case OS_RLIMIT_MSGQUEUE:
    case OS_RLIMIT_LOCKS:
    case OS_RLIMIT_RTTIME:
        out->current = out->maximum = OS_RLIM_INFINITY;
        return 0;
    default:
        return -1;
    }
}
