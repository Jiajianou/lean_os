/* user_space/libc/include/memory.h - M98
 *
 * The pre-ANSI name for <string.h>, alive only because code older than
 * the standard still includes it - binutils' testsuite named it first
 * here. Every system libc keeps this alias for the same reason; it
 * declares nothing of its own on any of them, and it never will here.
 */
#pragma once

#include <string.h>
