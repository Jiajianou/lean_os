#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t rip;
    uint64_t count;
    int32_t pid;
    int32_t _pad;
} prof_sample_t;

#define PROF_PID_KERNEL (-1)

typedef struct {
    uint64_t samples;
    uint64_t kernel;
    uint64_t user;
    uint64_t idle;
    uint64_t overflow;
    uint64_t distinct;
    int32_t running;
    int32_t _pad;
} prof_statistics_t;

typedef struct {
    uint64_t calls;
    uint64_t cycles;
} prof_syscall_counters_t;

#define PROFILE_OP_START     0
#define PROFILE_OP_STOP      1
#define PROFILE_OP_RESET     2
#define PROFILE_OP_STATISTICS     3
#define PROFILE_OP_SAMPLES   4
#define PROFILE_OP_SYSCALLS  5
#define PROFILE_OP_SYSRESET  6
#define PROFILE_OP_TIMING    7

#ifdef __cplusplus
}
#endif
