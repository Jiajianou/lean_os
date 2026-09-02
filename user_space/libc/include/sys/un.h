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

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[108];
};
