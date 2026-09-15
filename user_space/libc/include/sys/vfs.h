#pragma once

/* glibc's name for <sys/statfs.h>, which is where struct statfs and the two
   calls that fill it are declared. One header, two spellings, because
   portable code reaches for whichever one its author met first. */
#include <sys/statfs.h>
