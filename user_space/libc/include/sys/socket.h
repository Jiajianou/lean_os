/* user_space/libc/include/sys/socket.h - M89
 *
 * The BSD socket names, over the stack M27, M64 and M66 already built.
 *
 * M88's own bullet said the interesting thing about this file before it
 * existed: *"The stack is real; only the names are missing."* There is
 * Ethernet, ARP, IPv4, ICMP, UDP and a full TCP state machine under
 * here, reachable through SYS_socket and its six neighbours - what a
 * ported program cannot find is `socket(AF_INET, SOCK_STREAM, 0)`.
 *
 * **What is honest about this header and what is not.** The families and
 * types below are the standard numbers, and the ones this machine
 * implements are AF_INET with SOCK_STREAM and SOCK_DGRAM. AF_UNIX is
 * defined and `socket()` refuses it: M88 absorbed AF_UNIX and
 * socketpair into M100, where a multi-process browser needs them for a
 * reason, and a constant a program can name but never use is better than
 * a constant that silently gives it an Internet socket instead.
 */
#pragma once

#include <sys/types.h>
#include <stdint.h>

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

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

#define AF_UNSPEC 0
#define AF_UNIX   1   /* refused - M100. See the header note. */
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10  /* refused: there is no IPv6 in this stack */
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_LOCAL  AF_LOCAL
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3 /* refused: nothing here hands out raw frames */

/* The generic address. A program casts its family-specific struct to
 * this; `sockaddr_in` in <netinet/in.h> is the only one with anything
 * behind it. */
struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

/* Big enough for any address this system has, which is what a caller
 * allocating for accept() needs. */
struct sockaddr_storage {
    sa_family_t ss_family;
    char        __pad[126];
};

struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    int           msg_iovlen;
    void         *msg_control;
    socklen_t     msg_controllen;
    int           msg_flags;
};

/* send/recv flags. MSG_NOSIGNAL is honoured trivially - a write to a
 * closed socket here returns an error rather than raising SIGPIPE, so
 * asking for that behaviour asks for what already happens. The rest are
 * accepted and ignored, and each is ignorable: there is no out-of-band
 * data, no peek buffer and no non-blocking bit yet (M100). */
#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_WAITALL   0x08
#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000

/* setsockopt levels and names. See socket.c: this machine has no
 * per-socket options to set, so they are accepted where accepting is
 * harmless and refused where a program would act on the answer. */
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_LINGER    13
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

struct linger {
    int l_onoff;
    int l_linger;
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
/* M99: the ceiling a caller is told to expect from listen().
 *
 * 16, and it is the truth rather than a conventional 128: every socket
 * on this machine comes out of one pool of TCP_MAX_TCBS control blocks
 * (kernel/net/tcp.h), and a connection that arrives with no block free
 * is refused with a reset. So the number is not a per-listener backlog
 * at all - it is every connection this machine has, and a program that
 * asks for more is asking for something no part of this system can
 * hold. CPython's socketmodule.c names it with no #ifdef, which is how
 * it came to be defined here. */
#define SOMAXCONN 16

int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int setsockopt(int fd, int level, int option, const void *value, socklen_t len);
int getsockopt(int fd, int level, int option, void *value, socklen_t *len);

#ifdef __cplusplus
}
#endif
