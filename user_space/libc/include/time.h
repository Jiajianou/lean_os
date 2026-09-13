#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __lean_time_t_defined
#define __lean_time_t_defined
typedef long time_t;
#endif

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

typedef int clockid_t;
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int clock_gettime(clockid_t clk, struct timespec *ts);
int clock_getres(clockid_t clk, struct timespec *res);

int nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);

extern char *tzname[2];
extern long timezone;
extern int daylight;
void tzset(void);

char *strptime(const char *s, const char *format, struct tm *tm);

time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);

size_t strftime(char *out, size_t max, const char *fmt, const struct tm *tm);

char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *t);
char *ctime_r(const time_t *t, char *buf);

#ifndef __lean_clock_t_defined
#define __lean_clock_t_defined
typedef long clock_t;
#endif

#define CLOCKS_PER_SEC 1000

time_t time(time_t *out);
clock_t clock(void);

double difftime(time_t end, time_t start);

#ifdef __cplusplus
}
#endif
