#pragma once

#include <endian.h>
#include <limits.h>
#include <sys/types.h>

#define MAXPATHLEN PATH_MAX

#define NBBY 8

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x)   ((((x) - 1) & (x)) == 0)
