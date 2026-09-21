#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resource limits, and why they are a system call here.
   getrlimit(2) is a system call on every system that has it, and that is not
   an implementation detail a C library may decide differently: a caller is
   entitled to hand it a pointer it cannot write and be told EFAULT rather
   than be killed. Chromium's base::ProtectedMemory does exactly that on
   purpose - CheckMemoryReadOnly passes the address of a page it has just
   made read-only and requires the call to refuse - and a getrlimit that
   writes through the pointer from user space takes the signal instead.
   So the kernel answers, because the kernel is the only thing here that can
   look at a page table before it writes. */
typedef struct {
    uint64_t current;
    uint64_t maximum;
} os_rlimit_t;

#define OS_RLIM_INFINITY 0xFFFFFFFFFFFFFFFFULL

/* The same numbers <sys/resource.h> gives, because this is the ABI and that
   header is its C spelling. resource.c checks the two agree at compile time
   rather than trusting this comment. */
#define OS_RLIMIT_CPU        0
#define OS_RLIMIT_FSIZE      1
#define OS_RLIMIT_DATA       2
#define OS_RLIMIT_STACK      3
#define OS_RLIMIT_CORE       4
#define OS_RLIMIT_NOFILE     5
#define OS_RLIMIT_AS         6
#define OS_RLIMIT_NPROC      7
#define OS_RLIMIT_MEMLOCK    8
#define OS_RLIMIT_RSS        9
#define OS_RLIMIT_NICE      10
#define OS_RLIMIT_RTPRIO    11
#define OS_RLIMIT_SIGPENDING 12
#define OS_RLIMIT_MSGQUEUE  13
#define OS_RLIMIT_LOCKS     14
#define OS_RLIMIT_RTTIME    15
#define OS_RLIM_COUNT       16

#ifdef __cplusplus
}
#endif
