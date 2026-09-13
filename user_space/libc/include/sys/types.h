#pragma once

#include <stdint.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long          ssize_t;
typedef unsigned long ino_t;
typedef unsigned int  mode_t;
typedef long          off_t;
typedef int           pid_t;
typedef unsigned int  nlink_t;
typedef unsigned long dev_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned long blksize_t;
typedef unsigned long blkcnt_t;

#ifndef __lean_time_t_defined
#define __lean_time_t_defined
typedef long time_t;
#endif
#ifndef __lean_clock_t_defined
#define __lean_clock_t_defined
typedef long clock_t;
#endif
typedef long suseconds_t;

#ifdef __cplusplus
}
#endif
