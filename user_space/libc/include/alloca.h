/* user_space/libc/include/alloca.h - M98
 *
 * alloca is not a library function here and cannot be one anywhere: a
 * callee that returned would free the very bytes it was asked to
 * allocate. It has always been the compiler's intrinsic wearing a
 * function's name, and this header says so directly. GNU make's
 * file.c named it first.
 */
#pragma once

#define alloca(size) __builtin_alloca(size)
