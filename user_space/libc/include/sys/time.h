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

/* The timeval arithmetic every Unix has had since 4.2BSD. They are macros
   rather than functions everywhere, so software written against them uses
   them in places a function call cannot go - the condition of an if with no
   braces, most often - and each argument is a POINTER.
   `cmp` is spelled as an operator by the caller: timercmp(a, b, <).

   They name their arguments more than once, exactly as every other
   implementation of them does - glibc reads tvp twice in timerisset and
   writes through result three times in timeradd - so an argument with a
   side effect in it is the caller's bug. Avoiding that here would make
   these the one set on any Unix that behaved differently. */
#define timerisset(tvp)     ((tvp)->tv_sec || (tvp)->tv_usec)

#define timerclear(tvp)     ((tvp)->tv_sec = (tvp)->tv_usec = 0)

#define timercmp(a, b, cmp) \
    (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec cmp (b)->tv_usec) \
                                  : ((a)->tv_sec cmp (b)->tv_sec))

/* The carry is a while rather than an if on purpose: tv_usec is a long and
   nothing stops a caller handing over a timeval holding several seconds'
   worth of microseconds. */
#define timeradd(a, b, result)                                   \
    do {                                                         \
        (result)->tv_sec = (a)->tv_sec + (b)->tv_sec;            \
        (result)->tv_usec = (a)->tv_usec + (b)->tv_usec;         \
        while ((result)->tv_usec >= 1000000) {                   \
            (result)->tv_sec++;                                  \
            (result)->tv_usec -= 1000000;                        \
        }                                                        \
    } while (0)

#define timersub(a, b, result)                                   \
    do {                                                         \
        (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;            \
        (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;         \
        while ((result)->tv_usec < 0) {                          \
            (result)->tv_sec--;                                  \
            (result)->tv_usec += 1000000;                        \
        }                                                        \
    } while (0)

#ifdef __cplusplus
}
#endif

#include <sys/select.h>
