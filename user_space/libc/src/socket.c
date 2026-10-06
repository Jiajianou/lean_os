#include <sys/socket.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <stdlib.h>

#include "os_network.h"
#include "syscall_wrappers.h"

static int from_sockaddr(const struct sockaddr *sa, socklen_t length,
                         uint32_t *ip, uint16_t *port) {
    if (!sa || length < (socklen_t)sizeof(struct sockaddr_in)) {
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

static void to_sockaddr(struct sockaddr *sa, socklen_t *length,
                        uint32_t ip, uint16_t port) {
    if (!sa || !length) {
        return;
    }
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    in.sin_addr.s_addr = htonl(ip);
    socklen_t room = *length < (socklen_t)sizeof(in) ? *length : (socklen_t)sizeof(in);
    memcpy(sa, &in, room);
    *length = (socklen_t)sizeof(in);
}

static int un_name(const struct sockaddr *sa, socklen_t length,
                   const char **name_out, int *length_out) {
    size_t base = (size_t)(((struct sockaddr_un *)0)->sun_path);
    if (!sa || sa->sa_family != AF_UNIX || (size_t)length <= base) {
        return -1;
    }
    const struct sockaddr_un *un = (const struct sockaddr_un *)(const void *)sa;
    size_t n = (size_t)length - base;
    if (n > sizeof(un->sun_path)) {
        n = sizeof(un->sun_path);
    }
    if (un->sun_path[0] != '\0') {
        size_t i = 0;
        while (i < n && un->sun_path[i]) {
            i++;
        }
        n = i;
    }
    if (n == 0) {
        return -1;
    }
    *name_out = un->sun_path;
    *length_out = (int)n;
    return 0;
}

/* A program refused a socket, a program out of descriptors and a machine
   out of sockets are three different problems, and until M209 the second
   and third both arrived as EMFILE - a browser whose every connection the
   kernel had lingering was told it held too many descriptors. */
static int socket_errno(long code) {
    if (code == -OS_ERROR_ACCESS) {
        return EACCES;
    }
    if (code == -OS_ERROR_NFILE) {
        return ENFILE;
    }
    return EMFILE;
}

int socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain == AF_UNIX) {
        if (type != SOCK_STREAM && type != SOCK_SEQPACKET) {
            errno = ESOCKTNOSUPPORT;
            return -1;
        }
        long fd = sys_socket_in(type, OS_AF_UNIX);
        if (fd < 0) {
            errno = socket_errno(fd);
            return -1;
        }
        return (int)fd;
    }
    if (domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    int t;
    if (type == SOCK_STREAM) {
        t = OS_SOCKET_STREAM;
    } else if (type == SOCK_DGRAM) {
        t = OS_SOCKET_DGRAM;
    } else {
        errno = ESOCKTNOSUPPORT;
        return -1;
    }
    long fd = sys_socket(t);
    if (fd < 0) {
        errno = socket_errno(fd);
        return -1;
    }
    return (int)fd;
}

int bind(int fd, const struct sockaddr *address, socklen_t length) {
    const char *name;
    int namelen;
    if (un_name(address, length, &name, &namelen) == 0) {
        if (sys_bindun(fd, name, namelen) != 0) {
            errno = EADDRINUSE;
            return -1;
        }
        return 0;
    }
    uint32_t ip;
    uint16_t port;
    if (from_sockaddr(address, length, &ip, &port) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (sys_bind(fd, port) < 0) {
        errno = EADDRINUSE;
        return -1;
    }
    return 0;
}

int listen(int fd, int backlog) {
    (void)backlog;
    if (sys_listen(fd) < 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return 0;
}

int connect(int fd, const struct sockaddr *address, socklen_t length) {
    const char *name;
    int namelen;
    if (un_name(address, length, &name, &namelen) == 0) {
        if (sys_connectun(fd, name, namelen) != 0) {
            errno = ECONNREFUSED;
            return -1;
        }
        return 0;
    }
    uint32_t ip;
    uint16_t port;
    if (from_sockaddr(address, length, &ip, &port) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (sys_connect(fd, ip, port) < 0) {
        errno = ECONNREFUSED;
        return -1;
    }
    for (;;) {
        long st = sys_connstat(fd);
        if (st == 1) {
            return 0;
        }
        if (st < 0) {
            errno = ECONNREFUSED;
            return -1;
        }
        /* M202: a handshake is a network round trip, and yielding in a loop
           for it kept a processor busy for the whole of it. A millisecond's
           sleep between looks costs a connection at most that. */
        int wait_on = fd;
        sys_waitfds(&wait_on, 1, 1);
    }
}

int accept(int fd, struct sockaddr *address, socklen_t *length) {
    int nonblock = (sys_fcntl(fd, F_GETFL_COMMAND, 0) & O_NONBLOCK) != 0;
    for (;;) {
        os_sockaddr_t from;
        memset(&from, 0, sizeof(from));
        long nfd = sys_accept(fd, &from);
        if (nfd >= 0) {
            if (address && length) {
                to_sockaddr(address, length, from.ip, from.port);
            }
            return (int)nfd;
        }
        if (nfd == -OS_ERROR_MFILE || nfd == -OS_ERROR_NFILE) {
            errno = socket_errno(nfd);
            return -1;
        }
        if (nonblock) {
            errno = EAGAIN;
            return -1;
        }
        int wf = fd;
        sys_waitfds(&wf, 1, 1000);
    }
}

ssize_t send(int fd, const void *buffer, size_t length, int flags) {
    if (flags & MSG_DONTWAIT) {
        long n = sys_send(fd, buffer, (uint32_t)length);
        if (n < 0) {
            errno = EPIPE;
            return -1;
        }
        if (n == 0 && length > 0) {
            errno = EAGAIN;
            return -1;
        }
        return (ssize_t)n;
    }
    errno = 0;
    long n = write(fd, buffer, length);
    if (n < 0 && errno == 0) {
        errno = EPIPE;
    }
    return (ssize_t)n;
}

/* MSG_PEEK, which is a question rather than a read: what is queued, left
   where it is. Only the kernel can answer it - a C library that read the
   bytes and kept them would have taken them out of the socket, and the next
   reader (or a second descriptor for the same socket) would never see them.

   M183: this was forwarded to read(2) until then, so every peek ATE a byte.
   net::SocketPosix peeks one byte to ask whether a connection is still
   there, which Chromium does before reusing a socket, so one byte went
   missing from the middle of every TLS stream that already had data waiting
   - and TLS 1.3 always does, because the server sends session tickets the
   moment the handshake ends. BoringSSL then read a record header one byte
   late and killed the connection with a protocol_version alert. */
static ssize_t peek(int fd, void *buffer, size_t length, int flags) {
    long n = sys_peek(fd, buffer, (uint32_t)length, (flags & MSG_DONTWAIT) ? 1 : 0);
    if (n == -OS_ERROR_AGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (n == -OS_ERROR_INTR) {
        errno = EINTR;
        return -1;
    }
    if (n == -OS_ERROR_INVALID) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (n < 0) {
        errno = ENOTSOCK;
        return -1;
    }
    return (ssize_t)n;
}

ssize_t recv(int fd, void *buffer, size_t length, int flags) {
    if (flags & MSG_PEEK) {
        return peek(fd, buffer, length, flags);
    }
    if (flags & MSG_DONTWAIT) {
        long n = sys_receive(fd, buffer, (uint32_t)length);
        if (n < 0) {
            return 0;
        }
        if (n == 0 && length > 0) {
            errno = EAGAIN;
            return -1;
        }
        return (ssize_t)n;
    }
    return (ssize_t)read(fd, buffer, length);
}

ssize_t sendto(int fd, const void *buffer, size_t length, int flags,
               const struct sockaddr *to, socklen_t tolen) {
    (void)flags;
    if (!to) {
        return send(fd, buffer, length, flags);
    }
    uint32_t ip;
    uint16_t port;
    if (from_sockaddr(to, tolen, &ip, &port) != 0) {
        errno = EINVAL;
        return -1;
    }
    long n = sys_sendto(fd, ip, port, buffer, (uint32_t)length);
    if (n < 0) {
        errno = EHOSTUNREACH;
        return -1;
    }
    return (ssize_t)n;
}

ssize_t recvfrom(int fd, void *buffer, size_t length, int flags,
                 struct sockaddr *from, socklen_t *fromlen) {
    if (flags & MSG_PEEK) {
        /* A connected socket peeked at through recvfrom has no address to
           report that getpeername would not give, and this kernel's
           datagram queue is not peekable - so this is the stream case and
           the address is left alone. */
        return peek(fd, buffer, length, flags);
    }
    os_sockaddr_t sa;
    memset(&sa, 0, sizeof(sa));
    long n = sys_recvfrom(fd, buffer, (uint32_t)length, &sa);
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
    if (how < 0 || how > 2) {
        errno = EINVAL;
        return -1;
    }
    if (sys_sockshut(fd, how) == 0) {
        return 0;
    }
    errno = ENOSYS;
    return -1;
}

int socketpair(int domain, int type, int protocol, int file_descriptors[2]) {
    (void)protocol;
    if (domain != AF_UNIX) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (type != SOCK_STREAM && type != SOCK_SEQPACKET) {
        errno = ESOCKTNOSUPPORT;
        return -1;
    }
    if (!file_descriptors) {
        errno = EFAULT;
        return -1;
    }
    if (sys_socketpair(type, file_descriptors) != 0) {
        errno = EMFILE;
        return -1;
    }
    return 0;
}

#define MESSAGE_STAGE_MAX 4096

static int cmsg_collect_file_descriptors(const struct msghdr *message, int *out, int max) {
    int n = 0;
    const struct cmsghdr *c = CMSG_FIRSTHDR((struct msghdr *)message);
    while (c) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
            return -1;
        }
        size_t payload = c->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr));
        size_t count = payload / sizeof(int);
        const int *file_descriptors = (const int *)(const void *)CMSG_DATA((struct cmsghdr *)c);
        for (size_t i = 0; i < count; i++) {
            if (n >= max) {
                return -1;
            }
            out[n++] = file_descriptors[i];
        }
        c = CMSG_NXTHDR((struct msghdr *)message, (struct cmsghdr *)c);
    }
    return n;
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
    if (!message) {
        errno = EFAULT;
        return -1;
    }
    int file_descriptors[OS_MESSAGE_MAX_FILE_DESCRIPTORS];
    int nfds = cmsg_collect_file_descriptors(message, file_descriptors, OS_MESSAGE_MAX_FILE_DESCRIPTORS);
    if (nfds < 0) {
        errno = EINVAL;
        return -1;
    }
    unsigned char stage[MESSAGE_STAGE_MAX];
    const void *data = (const void *)0;
    size_t length = 0;
    if (message->msg_iovlen == 1 && message->msg_iov) {
        data = message->msg_iov[0].iov_base;
        length = message->msg_iov[0].iov_len;
    } else if (message->msg_iovlen > 1 && message->msg_iov) {
        for (int i = 0; i < message->msg_iovlen; i++) {
            size_t n = message->msg_iov[i].iov_len;
            if (length + n > sizeof(stage)) {
                n = sizeof(stage) - length;
            }
            memcpy(stage + length, message->msg_iov[i].iov_base, n);
            length += n;
            if (length == sizeof(stage)) {
                break;
            }
        }
        data = stage;
    }
    os_message_t m;
    memset(&m, 0, sizeof(m));
    m.data = (uint64_t)(uintptr_t)data;
    m.length = (uint32_t)length;
    m.nfds = (uint32_t)nfds;
    m.file_descriptors = (uint64_t)(uintptr_t)file_descriptors;
    long n = sys_sendmsg(fd, &m, flags);
    if (n == -OS_ERROR_AGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (n == -OS_ERROR_INTR) {
        errno = EINTR;
        return -1;
    }
    if (n < 0) {
        errno = EPIPE;
        return -1;
    }
    return (ssize_t)n;
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
    if (!message) {
        errno = EFAULT;
        return -1;
    }
    if (flags & MSG_PEEK) {
        /* The bytes, and no descriptors: a peek takes nothing, and handing
           the same descriptor over twice would be two references where the
           sender sent one. They stay queued for the recvmsg that follows. */
        ssize_t n = -1;
        if (message->msg_iovlen >= 1 && message->msg_iov) {
            n = peek(fd, message->msg_iov[0].iov_base,
                     message->msg_iov[0].iov_len, flags);
        } else {
            n = peek(fd, (void *)0, 0, flags);
        }
        if (n >= 0) {
            message->msg_controllen = 0;
            message->msg_namelen = 0;
            message->msg_flags = 0;
        }
        return n;
    }
    int file_descriptors[OS_MESSAGE_MAX_FILE_DESCRIPTORS];
    int max_file_descriptors = 0;
    if (message->msg_control && (size_t)message->msg_controllen > CMSG_LEN(0)) {
        size_t room = ((size_t)message->msg_controllen - CMSG_LEN(0)) / sizeof(int);
        max_file_descriptors = room > OS_MESSAGE_MAX_FILE_DESCRIPTORS ? OS_MESSAGE_MAX_FILE_DESCRIPTORS : (int)room;
    }
    unsigned char stage[MESSAGE_STAGE_MAX];
    void *data = (void *)0;
    size_t length = 0;
    int scatter = 0;
    if (message->msg_iovlen == 1 && message->msg_iov) {
        data = message->msg_iov[0].iov_base;
        length = message->msg_iov[0].iov_len;
    } else if (message->msg_iovlen > 1 && message->msg_iov) {
        for (int i = 0; i < message->msg_iovlen; i++) {
            length += message->msg_iov[i].iov_len;
        }
        if (length > sizeof(stage)) {
            length = sizeof(stage);
        }
        data = stage;
        scatter = 1;
    }
    os_message_t m;
    memset(&m, 0, sizeof(m));
    m.data = (uint64_t)(uintptr_t)data;
    m.length = (uint32_t)length;
    m.nfds = (uint32_t)max_file_descriptors;
    m.file_descriptors = (uint64_t)(uintptr_t)file_descriptors;
    long n = sys_recvmsg(fd, &m, flags);
    if (n == -OS_ERROR_AGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (n == -OS_ERROR_INTR) {
        errno = EINTR;
        return -1;
    }
    if (n < 0) {
        errno = ENOTSOCK;
        return -1;
    }
    if (scatter) {
        size_t off = 0;
        for (int i = 0; i < message->msg_iovlen && off < (size_t)n; i++) {
            size_t take = message->msg_iov[i].iov_len;
            if (take > (size_t)n - off) {
                take = (size_t)n - off;
            }
            memcpy(message->msg_iov[i].iov_base, stage + off, take);
            off += take;
        }
    }
    message->msg_flags = 0;
    if (m.flags & OS_MESSAGE_TRUNC) {
        message->msg_flags |= MSG_TRUNC;
    }
    if (m.flags & OS_MESSAGE_CTRUNC) {
        message->msg_flags |= MSG_CTRUNC;
    }
    if (m.nfds > 0 && message->msg_control) {
        struct cmsghdr *c = (struct cmsghdr *)message->msg_control;
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(m.nfds * sizeof(int));
        memcpy(CMSG_DATA(c), file_descriptors, m.nfds * sizeof(int));
        message->msg_controllen = (socklen_t)c->cmsg_len;
    } else {
        message->msg_controllen = 0;
    }
    message->msg_namelen = 0;
    return (ssize_t)n;
}

/* M226: an AF_UNIX socket's name, as Linux reports it - the family alone for
   an unnamed socket, the path and its terminating zero for a pathname, and
   the bytes as bound for an abstract name. Truncated to the caller's buffer,
   with the full length reported, as getsockname(2) says. */
static void to_sockaddr_un(struct sockaddr *address, socklen_t *length,
                           const os_socket_identity_t *identity) {
    size_t base = (size_t)(((struct sockaddr_un *)0)->sun_path);
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    size_t n = identity->name_length;
    if (n > sizeof(un.sun_path)) {
        n = sizeof(un.sun_path);
    }
    memcpy(un.sun_path, identity->name, n);
    size_t full = base + n;
    if (n > 0 && identity->name[0] != '\0' && n < sizeof(un.sun_path)) {
        full++;
    }
    if (address && length) {
        size_t room = (size_t)*length < sizeof(un) ? (size_t)*length : sizeof(un);
        memcpy(address, &un, room);
    }
    if (length) {
        *length = (socklen_t)full;
    }
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length) {
    os_socket_identity_t identity;
    if (sys_sockident(fd, &identity, 0) == 0 && identity.family == OS_AF_UNIX) {
        to_sockaddr_un(address, length, &identity);
        return 0;
    }
    os_sockaddr_t local;
    if (sys_sockname(fd, &local) != 0) {
        errno = ENOTSOCK;
        return -1;
    }
    to_sockaddr(address, length, local.ip, local.port);
    return 0;
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length) {
    os_socket_identity_t identity;
    long kind = sys_sockident(fd, &identity, 1);
    if (kind == -2) {
        errno = ENOTCONN;
        return -1;
    }
    if (kind == 0 && identity.family == OS_AF_UNIX) {
        to_sockaddr_un(address, length, &identity);
        return 0;
    }
    os_sockaddr_t peer;
    long r = sys_peername(fd, &peer);
    if (r == -2) {
        errno = ENOTCONN;
        return -1;
    }
    if (r != 0) {
        errno = ENOTSOCK;
        return -1;
    }
    to_sockaddr(address, length, peer.ip, peer.port);
    return 0;
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
    if (level == IPPROTO_TCP && option == TCP_NODELAY) {
        int on = value ? *(const int *)value : 0;
        if (on) {
            return 0;
        }
        errno = ENOPROTOOPT;
        return -1;
    }
    /* M226: kept by the kernel on the socket, and what bind() consults.
       It used to be accepted and kept nowhere - a program asking for it was
       told it had it, then refused its own port while the connections it
       had just closed sat in TIME_WAIT. */
    if (level == SOL_SOCKET && option == SO_REUSEADDR) {
        if (!value || length < (socklen_t)sizeof(int)) {
            errno = EINVAL;
            return -1;
        }
        if (sys_sockopt(fd, OS_SOCKOPT_REUSEADDR, *(const int *)value ? 1 : 0) != 0) {
            errno = ENOPROTOOPT;
            return -1;
        }
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    if (level == SOL_SOCKET && option == SO_PEERCRED && value && length &&
        *length >= (socklen_t)sizeof(struct ucred)) {
        os_ucred_t c;
        if (sys_unix_peer_credentials(fd, &c) != 0) {
            errno = ENOTCONN;
            return -1;
        }
        struct ucred *out = (struct ucred *)value;
        out->pid = (pid_t)c.pid;
        out->uid = (uid_t)c.uid;
        out->gid = (gid_t)c.gid;
        *length = (socklen_t)sizeof(struct ucred);
        return 0;
    }
    if (level == SOL_SOCKET && option == SO_REUSEADDR && value && length &&
        *length >= (socklen_t)sizeof(int)) {
        long on = sys_sockopt(fd, OS_SOCKOPT_REUSEADDR, -1);
        if (on < 0) {
            errno = ENOPROTOOPT;
            return -1;
        }
        *(int *)value = (int)on;
        *length = (socklen_t)sizeof(int);
        return 0;
    }
    if (level == SOL_SOCKET && option == SO_ERROR && value && length &&
        *length >= (socklen_t)sizeof(int)) {
        *(int *)value = 0;
        *length = (socklen_t)sizeof(int);
        return 0;
    }
    /* M226: the socket's own type. It was SOCK_STREAM for every socket,
       so a UDP socket described itself as a stream. */
    if (level == SOL_SOCKET && option == SO_TYPE && value && length &&
        *length >= (socklen_t)sizeof(int)) {
        os_socket_identity_t identity;
        if (sys_sockident(fd, &identity, 0) != 0) {
            errno = ENOTSOCK;
            return -1;
        }
        if (identity.family == OS_AF_UNIX) {
            *(int *)value = (int)identity.type;
        } else {
            *(int *)value = identity.type == OS_SOCKET_DGRAM ? SOCK_DGRAM : SOCK_STREAM;
        }
        *length = (socklen_t)sizeof(int);
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;
