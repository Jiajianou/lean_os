/* user_space/libc/src/socket.c - M89
 *
 * The BSD socket names over SYS_socket and its neighbours. See
 * <sys/socket.h> for what is real here and what is refused.
 *
 * ---- the one genuine piece of work in this file ----------------------
 *
 * Byte order. `sockaddr_in` carries its address and port in network
 * order; this kernel's ABI carries them in host order, deliberately and
 * with a stated reason (system_api/include/os_net.h: "this OS never
 * byte-swaps an address into a register, so an on-wire order here would
 * be a second representation to get wrong"). Both are right for their
 * side, and this file is the single seam where they meet. Every
 * conversion is in `from_sockaddr` and `to_sockaddr` so there is one
 * place to be wrong rather than eleven.
 */
#include <sys/socket.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>  /* M100: O_NONBLOCK for a blocking accept */
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <unistd.h>  /* M100: read and write, which recv and send are now */
#include <sys/un.h>

#include "os_net.h" /* system_api/include/os_net.h - OS_SOCK_*, os_sockaddr_t */
#include "syscall_wrappers.h"

/* network-order sockaddr_in -> host-order (ip, port) */
static int from_sockaddr(const struct sockaddr *sa, socklen_t len,
                         uint32_t *ip, uint16_t *port) {
    if (!sa || len < (socklen_t)sizeof(struct sockaddr_in)) {
        return -1;
    }
    if (sa->sa_family != AF_INET) {
        return -1;
    }
    const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)sa;
    *ip = ntohl(in->sin_addr.s_addr);
    *port = ntohs(in->sin_port);
    return 0;
}

static void to_sockaddr(struct sockaddr *sa, socklen_t *len,
                        uint32_t ip, uint16_t port) {
    if (!sa || !len) {
        return;
    }
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    in.sin_addr.s_addr = htonl(ip);
    socklen_t room = *len < (socklen_t)sizeof(in) ? *len : (socklen_t)sizeof(in);
    memcpy(sa, &in, room);
    *len = (socklen_t)sizeof(in); /* the size it WOULD have needed - the documented contract */
}

int socket(int domain, int type, int protocol) {
    (void)protocol; /* the type already selects TCP or UDP here */
    if (domain != AF_INET) {
        /* AF_UNIX is M100's and AF_INET6 does not exist in this stack.
         * Refused rather than quietly given an Internet socket, which
         * would connect somewhere real and surprising. */
        errno = EAFNOSUPPORT;
        return -1;
    }
    int t;
    if (type == SOCK_STREAM) {
        t = OS_SOCK_STREAM;
    } else if (type == SOCK_DGRAM) {
        t = OS_SOCK_DGRAM;
    } else {
        errno = ESOCKTNOSUPPORT;
        return -1;
    }
    long fd = sys_socket(t);
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }
    return (int)fd;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    uint32_t ip;
    uint16_t port;
    if (from_sockaddr(addr, len, &ip, &port) != 0) {
        errno = EINVAL;
        return -1;
    }
    /* The address half is dropped, and that is honest rather than lazy:
     * this machine has one interface, so binding to INADDR_ANY and
     * binding to its address are the same thing, and there is no second
     * address a caller could have meant. */
    if (sys_bind(fd, port) < 0) {
        errno = EADDRINUSE;
        return -1;
    }
    return 0;
}

int listen(int fd, int backlog) {
    (void)backlog; /* the kernel's queue is a fixed size; asking for more is not refused, it is just not granted */
    if (sys_listen(fd) < 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return 0;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    uint32_t ip;
    uint16_t port;
    if (from_sockaddr(addr, len, &ip, &port) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (sys_connect(fd, ip, port) < 0) {
        errno = ECONNREFUSED;
        return -1;
    }
    /* SYS_connect returns once the SYN is away - its own ABI note is
     * explicit that it is NOT "connected". POSIX connect() on a blocking
     * socket returns when the handshake completes, so this waits, which
     * is the difference between a program that works and one that writes
     * into a connection that does not exist yet. */
    for (;;) {
        long st = sys_connstat(fd);
        if (st == 1) {
            return 0;
        }
        if (st < 0) {
            errno = ECONNREFUSED;
            return -1;
        }
        sys_yield();
    }
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    /* ---- M100: accept blocks, like every other libc's ----------------
     *
     * SYS_accept never blocks - it returns -1 when the listener has no
     * completed connection waiting - which is right for the poll-driven
     * servers this project wrote (httpd loops on SYS_sockpoll) and wrong
     * for a program written against POSIX, which calls accept() and
     * expects to wait. mbedtls's server is that program: its
     * net_accept() calls accept() once and treats -1 as fatal. So this
     * waits, on the same poll channel a socket read waits on - a
     * listener is "readable" exactly when an accept would succeed
     * (kernel socket_pending) - unless the descriptor is non-blocking,
     * in which case it is EAGAIN as before. */
    int nonblock = (sys_fcntl(fd, F_GETFL_CMD, 0) & O_NONBLOCK) != 0;
    for (;;) {
        os_sockaddr_t from;
        memset(&from, 0, sizeof(from));
        long nfd = sys_accept(fd, &from);
        if (nfd >= 0) {
            if (addr && len) {
                to_sockaddr(addr, len, from.ip, from.port);
            }
            return (int)nfd;
        }
        if (nonblock) {
            errno = EAGAIN;
            return -1;
        }
        /* Block until the listener is readable. A wake for something
         * else costs one more sys_accept that finds nothing; a spurious
         * wake or a lost race just loops. */
        int wf = fd;
        sys_waitfds(&wf, 1, 1000);
    }
}

/* ---- M100: send and recv block, like every other libc's ----------------
 *
 * Until M100 these were SYS_send and SYS_recv with the names changed,
 * and SYS_recv's "0 if nothing is waiting right now" reached programs
 * as recv()'s 0 - which every program written against POSIX reads as
 * the end of the stream. Nothing here noticed because nothing here
 * called recv() without polling first. mbedtls did.
 *
 * So the ordinary path is now write(2) and read(2), which block in the
 * kernel (or say EAGAIN under O_NONBLOCK), and the one flag that asks
 * for the old behaviour - MSG_DONTWAIT - is answered by the old calls,
 * with their 0 turned into the EAGAIN it always meant. */
ssize_t send(int fd, const void *buf, size_t len, int flags) {
    if (flags & MSG_DONTWAIT) {
        long n = sys_send(fd, buf, (uint32_t)len);
        if (n < 0) {
            errno = EPIPE;
            return -1;
        }
        if (n == 0 && len > 0) {
            errno = EAGAIN;
            return -1;
        }
        return (ssize_t)n;
    }
    errno = 0;
    long n = write(fd, buf, len);
    if (n < 0 && errno == 0) {
        errno = EPIPE; /* the connection is gone, and this ABI carries no finer reason */
    }
    return (ssize_t)n;
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    if (flags & MSG_DONTWAIT) {
        long n = sys_recv(fd, buf, (uint32_t)len);
        if (n < 0) {
            return 0; /* SYS_recv's -1 is end of stream, which recv() reports as 0 */
        }
        if (n == 0 && len > 0) {
            errno = EAGAIN;
            return -1;
        }
        return (ssize_t)n;
    }
    return (ssize_t)read(fd, buf, len);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen) {
    (void)flags;
    if (!to) {
        return send(fd, buf, len, flags);
    }
    uint32_t ip;
    uint16_t port;
    if (from_sockaddr(to, tolen, &ip, &port) != 0) {
        errno = EINVAL;
        return -1;
    }
    long n = sys_sendto(fd, ip, port, buf, (uint32_t)len);
    if (n < 0) {
        errno = EHOSTUNREACH;
        return -1;
    }
    return (ssize_t)n;
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen) {
    (void)flags;
    os_sockaddr_t sa;
    memset(&sa, 0, sizeof(sa));
    long n = sys_recvfrom(fd, buf, (uint32_t)len, &sa);
    if (n < 0) {
        errno = EAGAIN;
        return -1;
    }
    if (from && fromlen) {
        to_sockaddr(from, fromlen, sa.ip, sa.port);
    }
    return (ssize_t)n;
}

int shutdown(int fd, int how) {
    /* There is no half-close in this stack: SYS_close is the only way to
     * end a connection, and calling it here would close a descriptor the
     * caller still holds. Refused rather than silently doing nothing,
     * because a program that shuts down its write side and then waits
     * for the peer's EOF would wait forever. */
    (void)fd;
    (void)how;
    errno = ENOSYS;
    return -1;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    /* This machine's own address, which SYS_netconf reports. The port is
     * not available - nothing in the ABI reports a socket's bound port
     * back - so it is 0, which is what an unbound socket reads as. */
    (void)fd;
    os_netconf_t nc;
    if (sys_netconf(&nc) != 0) {
        errno = ENOTSOCK;
        return -1;
    }
    to_sockaddr(addr, len, nc.ip, 0);
    return 0;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd;
    (void)addr;
    (void)len;
    errno = ENOTCONN; /* nothing in the ABI reports a connection's peer back */
    return -1;
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t len) {
    (void)fd;
    (void)len;
    if (level == IPPROTO_TCP && option == TCP_NODELAY) {
        /* Granted when it asks to DISABLE Nagle, refused when it asks to
         * enable it - see <netinet/tcp.h>. This stack has no Nagle, so
         * "do not buffer" is already true and "do buffer" is not
         * something it can start doing. */
        int on = value ? *(const int *)value : 0;
        if (on) {
            return 0; /* NODELAY on == do not buffer == what already happens */
        }
        errno = ENOPROTOOPT;
        return -1;
    }
    if (level == SOL_SOCKET && option == SO_REUSEADDR) {
        /* A port is released when its socket closes and there is no
         * TIME_WAIT hold on rebinding here, so a program asking to reuse
         * an address is asking for what it already gets. */
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int getsockopt(int fd, int level, int option, void *value, socklen_t *len) {
    (void)fd;
    if (level == SOL_SOCKET && option == SO_ERROR && value && len &&
        *len >= (socklen_t)sizeof(int)) {
        /* The pending error, which is how a program checks a connect()
         * that it started. There is no per-socket error to report here,
         * and 0 means "no error" - true for a socket that reached this
         * call, because connect() above does not return until the
         * handshake either completed or failed. */
        *(int *)value = 0;
        *len = (socklen_t)sizeof(int);
        return 0;
    }
    if (level == SOL_SOCKET && option == SO_TYPE && value && len &&
        *len >= (socklen_t)sizeof(int)) {
        /* M100: mbedtls's net_accept asks this to tell a TCP listener
         * from a UDP one. Every socket that reaches accept() on this
         * machine is SOCK_STREAM - the TLS server binds TCP, and DTLS
         * (the one caller that accepts on a datagram socket) is not
         * ported here - so that is the honest answer for this code path.
         * A datagram socket that somehow reached here would be
         * mislabelled; nothing does, and this note is the record that
         * the day one might, this is where it is made real. */
        *(int *)value = SOCK_STREAM;
        *len = (socklen_t)sizeof(int);
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

/* M89: the two well-known v6 addresses, defined so that a program that
 * references them links. Nothing in this stack will ever hand one to the
 * kernel - see <netinet/in.h> for why declaring the family without
 * implementing it is a different thing from pretending it works. */
const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;
