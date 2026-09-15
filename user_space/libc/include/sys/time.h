#pragma once

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tz);

int settimeofday(const struct timeval *tv, const void *tz);

int utimes(const char *path, const struct timeval tv[2]);
int futimes(int fd, const struct timeval tv[2]);

#ifdef __cplusplus
}
#endif

#include <sys/select.h>
