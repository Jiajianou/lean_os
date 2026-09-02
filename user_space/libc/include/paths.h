/* user_space/libc/include/paths.h - M89
 *
 * The handful of paths a ported program hardcodes by macro rather than
 * by string. Each one is what this machine actually has, which is the
 * only reason to define it: a _PATH_ macro pointing somewhere that does
 * not exist is worse than a program failing to compile, because it
 * fails at run time and blames the wrong thing.
 */
#pragma once

/* system_api/include/paths.h has the same name as this file and sits
 * later on the same include path, and it carries something different and
 * still needed: PATH_MAX_LEN and the PATH_BIN/PATH_ETC/PATH_TMP names
 * this OS's own programs are written against. libc/src/dirent.c includes
 * "paths.h" for exactly those, so without this line adding a
 * <paths.h> to libc would have silently shadowed them and broken a file
 * that has nothing to do with this milestone. Same tool and same reason
 * as <signal.h> and <termios.h> in this directory. */
#include_next <paths.h>

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

#define _PATH_BSHELL  "/bin/sh"
#define _PATH_DEVNULL "/dev/null"   /* M87's devfs */
#define _PATH_TTY     "/dev/tty"    /* M85's terminal device */
#define _PATH_CONSOLE "/dev/console"
#define _PATH_TMP     "/tmp/"
#define _PATH_STDPATH "/bin"        /* there is one directory of programs here */
#define _PATH_DEFPATH "/bin"

#ifdef __cplusplus
}
#endif
