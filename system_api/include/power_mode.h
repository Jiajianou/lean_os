/* system_api/include/power_mode.h
 *
 * M47: SYS_shutdown's one argument, shared between the kernel
 * (kernel/power/power.h) and everything that can ask for a shutdown -
 * compositor.c's Power controls, and the `shutdown`/`reboot` commands.
 *
 * Its own header rather than a pair of #defines in syscall.h because the
 * compositor and the two commands include it for the constants without
 * wanting the syscall numbering, and because "power_mode.h" is a name
 * that cannot collide with anything on either include path - the same
 * care wm.h's own header comment describes.
 */
#pragma once

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

#define POWER_OFF    0
#define POWER_REBOOT 1

#ifdef __cplusplus
}
#endif
