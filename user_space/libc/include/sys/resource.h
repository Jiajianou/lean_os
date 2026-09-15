#pragma once

#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NOFILE  5
#define RLIMIT_AS      6
#define RLIMIT_NPROC   7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_RSS     9
#define RLIMIT_NICE    10
#define RLIMIT_RTPRIO  11
#define RLIMIT_SIGPENDING 12
#define RLIMIT_MSGQUEUE 13
#define RLIMIT_LOCKS   14
#ifndef RLIMIT_RTTIME
#define RLIMIT_RTTIME  15
#endif
#define RLIM_NLIMITS   16

/* This machine has one nice value and it is zero. getpriority reports it;
   setpriority accepts zero and refuses everything else, because M65's rule
   is that a call which pretends to enforce something is worse than one that
   fails honestly - this scheduler has two priority CLASSES, not a range. */
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int value);

#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

typedef uint64_t rlim_t;

#define RLIM_INFINITY ((rlim_t)-1)

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

int getrusage(int who, struct rusage *usage);
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);

#ifdef __cplusplus
}
#endif
