#pragma once

#include <stdint.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long          ssize_t;

/* The BSD spellings. They are not POSIX and they are in every C library
   that has ever been asked for them, because thirty years of network code
   writes u_char. */
typedef unsigned char      u_char;
typedef unsigned short     u_short;
typedef unsigned int       u_int;
typedef unsigned long      u_long;
typedef unsigned char      u_int8_t;
typedef unsigned short     u_int16_t;
typedef unsigned int       u_int32_t;
typedef unsigned long long u_int64_t;
typedef char              *caddr_t;
typedef unsigned long ino_t;
typedef unsigned int  mode_t;
typedef long          off_t;
/* M161. off_t is already 64 bits here, so off64_t is not a wider type - it is
   the same one under the name glibc gave it when off_t was not. See the note
   above the LFS64 declarations in <stdio.h>. */
typedef long          off64_t;
typedef int           pid_t;
typedef unsigned int  nlink_t;
typedef unsigned long dev_t;
/* Wide enough to hold a pid_t, a uid_t or a gid_t, which is all POSIX
   asks of it; getpriority and setpriority are what name it here. */
typedef unsigned int  id_t;
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
typedef unsigned int useconds_t;

#ifdef __cplusplus
}
#endif
