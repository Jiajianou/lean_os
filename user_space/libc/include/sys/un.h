/* user_space/libc/include/sys/un.h - M89, made real in M118
 *
 * The Unix-domain address, and as of M118 an address a program can
 * actually connect: see kernel/ipc/unixsock.h for the family and
 * docs/browser.md for why it was built before any engine that needs it
 * was chosen.
 *
 * **What a path in `sun_path` is here.** A name in a kernel table, not a
 * node in leanfs - nothing appears in the directory, `stat()` on it fails
 * and `unlink()` does not unbind it. That is what kernel/ipc/pipe.c's
 * named pipes have always been, it is enough for two programs that agree
 * on a string, and the condition for making it a real filesystem node is
 * written down at unixsock_bind rather than left to be discovered.
 *
 * **The abstract namespace works**, and is the better choice on this
 * machine for exactly the reason above: a name whose first byte is NUL
 * was never a filesystem path in any Unix, so nothing about it is a
 * half-truth here. Linux's programs use it; Chromium's sandbox does.
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

/* The length to pass bind()/connect() for a NUL-terminated path, which is
 * what every program that uses it does. Not usable for an abstract name,
 * whose first byte is the NUL - pass the real length there. */
#define SUN_LEN(p) ((size_t)(((struct sockaddr_un *)0)->sun_path) + strlen((p)->sun_path))

#ifdef __cplusplus
}
#endif
