#pragma once

#include_next <paths.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _PATH_BSHELL  "/bin/sh"
#define _PATH_DEVNULL "/dev/null"
#define _PATH_TTY     "/dev/tty"
#define _PATH_CONSOLE "/dev/console"
#define _PATH_TMP     "/tmp/"
#define _PATH_STDPATH "/bin"
#define _PATH_DEFPATH "/bin"

#ifdef __cplusplus
}
#endif
