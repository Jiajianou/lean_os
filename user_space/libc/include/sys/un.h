/* user_space/libc/include/sys/un.h - M89
 *
 * The Unix-domain address, for programs that mention it.
 *
 * There are no AF_UNIX sockets here: M88 absorbed them into M100, where
 * a multi-process browser needs them for a reason rather than for
 * completeness. `socket(AF_UNIX, ...)` is refused, so this struct is
 * something a program can declare and not something it can connect. The
 * header exists because code that has a Unix-domain path it never takes
 * still has to compile.
 */
#pragma once

#include <sys/socket.h>

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

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[108];
};

#ifdef __cplusplus
}
#endif
