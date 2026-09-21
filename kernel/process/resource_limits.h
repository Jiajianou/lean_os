#pragma once

#include <stdint.h>

#include "os_resource.h"

/* What this machine's limits are, in one place that a test can compile.

   They are the kernel's own numbers rather than the C library's, because the
   kernel is what enforces them: a descriptor table is a fixed array in the
   task, the task table is a fixed array in the scheduler, and the stack is
   what the loader laid out. A limit answered from a constant in a header a
   program was linked against is a claim about a kernel that might not be the
   one running it.

   Nothing here can move, and that is why there is no setter: see
   sys_setrlimit, which compares and refuses rather than accepting and
   changing nothing. */
int resource_limit_for(uint64_t resource, os_rlimit_t *out);
