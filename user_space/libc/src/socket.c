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

#include "os_net.h"
#include "syscall_wrappers.h"

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
    *len = (socklen_t)sizeof(in);
}

static int un_name(const struct sockaddr *sa, socklen_t len,
                   const char **name_out, int *len_out) {
    size_t base = (size_t)(((struct sockaddr_un *)0)->sun_path);
    if (!sa || sa->sa_family != AF_UNIX || (size_t)len <= base) {
        return -1;
    }
    const struct sockaddr_un *un = (const struct sockaddr_un *)(const void *)sa;
    size_t n = (size_t)len - base;
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
    *len_out = (int)n;
    return 0;
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
            errno = EMFILE;
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
    const char *name;
    int namelen;
    if (un_name(addr, len, &name, &namelen) == 0) {
        if (sys_bindun(fd, name, namelen) != 0) {
            errno = EADDRINUSE;
            return -1;
        }
        return 0;
    }
    uint32_t ip;
    uint16_t port;
    if (from_sockaddr(addr, len, &ip, &port) != 0) {
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

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    const char *name;
    int namelen;
    if (un_name(addr, len, &name, &namelen) == 0) {
        if (sys_connectun(fd, name, namelen) != 0) {
            errno = ECONNREFUSED;
            return -1;
        }
        return 0;
    }
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
        int wf = fd;
        sys_waitfds(&wf, 1, 1000);
    }
}

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
        errno = EPIPE;
    }
    return (ssize_t)n;
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    if (flags & MSG_DONTWAIT) {
        long n = sys_recv(fd, buf, (uint32_t)len);
        if (n < 0) {
            return 0;
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

int socketpair(int domain, int type, int protocol, int fds[2]) {
    (void)protocol;
    if (domain != AF_UNIX) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (type != SOCK_STREAM && type != SOCK_SEQPACKET) {
        errno = ESOCKTNOSUPPORT;
        return -1;
    }
    if (!fds) {
        errno = EFAULT;
        return -1;
    }
    if (sys_socketpair(type, fds) != 0) {
        errno = EMFILE;
        return -1;
    }
    return 0;
}

#define MSG_STAGE_MAX 4096

static int cmsg_collect_fds(const struct msghdr *msg, int *out, int max) {
    int n = 0;
    const struct cmsghdr *c = CMSG_FIRSTHDR((struct msghdr *)msg);
    while (c) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
            return -1;
        }
        size_t payload = c->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr));
        size_t count = payload / sizeof(int);
        const int *fds = (const int *)(const void *)CMSG_DATA((struct cmsghdr *)c);
        for (size_t i = 0; i < count; i++) {
            if (n >= max) {
                return -1;
            }
            out[n++] = fds[i];
        }
        c = CMSG_NXTHDR((struct msghdr *)msg, (struct cmsghdr *)c);
    }
    return n;
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    if (!msg) {
        errno = EFAULT;
        return -1;
    }
    int fds[OS_MSG_MAX_FDS];
    int nfds = cmsg_collect_fds(msg, fds, OS_MSG_MAX_FDS);
    if (nfds < 0) {
        errno = EINVAL;
        return -1;
    }
    unsigned char stage[MSG_STAGE_MAX];
    const void *data = (const void *)0;
    size_t len = 0;
    if (msg->msg_iovlen == 1 && msg->msg_iov) {
        data = msg->msg_iov[0].iov_base;
        len = msg->msg_iov[0].iov_len;
    } else if (msg->msg_iovlen > 1 && msg->msg_iov) {
        for (int i = 0; i < msg->msg_iovlen; i++) {
            size_t n = msg->msg_iov[i].iov_len;
            if (len + n > sizeof(stage)) {
                n = sizeof(stage) - len;
            }
            memcpy(stage + len, msg->msg_iov[i].iov_base, n);
            len += n;
            if (len == sizeof(stage)) {
                break;
            }
        }
        data = stage;
    }
    os_msg_t m;
    memset(&m, 0, sizeof(m));
    m.data = (uint64_t)(uintptr_t)data;
    m.len = (uint32_t)len;
    m.nfds = (uint32_t)nfds;
    m.fds = (uint64_t)(uintptr_t)fds;
    long n = sys_sendmsg(fd, &m, flags);
    if (n == -OS_ERR_AGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (n == -OS_ERR_INTR) {
        errno = EINTR;
        return -1;
    }
    if (n < 0) {
        errno = EPIPE;
        return -1;
    }
    return (ssize_t)n;
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
    if (!msg) {
        errno = EFAULT;
        return -1;
    }
    int fds[OS_MSG_MAX_FDS];
    int max_fds = 0;
    if (msg->msg_control && (size_t)msg->msg_controllen > CMSG_LEN(0)) {
        size_t room = ((size_t)msg->msg_controllen - CMSG_LEN(0)) / sizeof(int);
        max_fds = room > OS_MSG_MAX_FDS ? OS_MSG_MAX_FDS : (int)room;
    }
    unsigned char stage[MSG_STAGE_MAX];
    void *data = (void *)0;
    size_t len = 0;
    int scatter = 0;
    if (msg->msg_iovlen == 1 && msg->msg_iov) {
        data = msg->msg_iov[0].iov_base;
        len = msg->msg_iov[0].iov_len;
    } else if (msg->msg_iovlen > 1 && msg->msg_iov) {
        for (int i = 0; i < msg->msg_iovlen; i++) {
            len += msg->msg_iov[i].iov_len;
        }
        if (len > sizeof(stage)) {
            len = sizeof(stage);
        }
        data = stage;
        scatter = 1;
    }
    os_msg_t m;
    memset(&m, 0, sizeof(m));
    m.data = (uint64_t)(uintptr_t)data;
    m.len = (uint32_t)len;
    m.nfds = (uint32_t)max_fds;
    m.fds = (uint64_t)(uintptr_t)fds;
    long n = sys_recvmsg(fd, &m, flags);
    if (n == -OS_ERR_AGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (n == -OS_ERR_INTR) {
        errno = EINTR;
        return -1;
    }
    if (n < 0) {
        errno = ENOTSOCK;
        return -1;
    }
    if (scatter) {
        size_t off = 0;
        for (int i = 0; i < msg->msg_iovlen && off < (size_t)n; i++) {
            size_t take = msg->msg_iov[i].iov_len;
            if (take > (size_t)n - off) {
                take = (size_t)n - off;
            }
            memcpy(msg->msg_iov[i].iov_base, stage + off, take);
            off += take;
        }
    }
    msg->msg_flags = 0;
    if (m.flags & OS_MSG_TRUNC) {
        msg->msg_flags |= MSG_TRUNC;
    }
    if (m.flags & OS_MSG_CTRUNC) {
        msg->msg_flags |= MSG_CTRUNC;
    }
    if (m.nfds > 0 && msg->msg_control) {
        struct cmsghdr *c = (struct cmsghdr *)msg->msg_control;
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(m.nfds * sizeof(int));
        memcpy(CMSG_DATA(c), fds, m.nfds * sizeof(int));
        msg->msg_controllen = (socklen_t)c->cmsg_len;
    } else {
        msg->msg_controllen = 0;
    }
    msg->msg_namelen = 0;
    return (ssize_t)n;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
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
    errno = ENOTCONN;
    return -1;
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t len) {
    (void)fd;
    (void)len;
    if (level == IPPROTO_TCP && option == TCP_NODELAY) {
        int on = value ? *(const int *)value : 0;
        if (on) {
            return 0;
        }
        errno = ENOPROTOOPT;
        return -1;
    }
    if (level == SOL_SOCKET && option == SO_REUSEADDR) {
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int getsockopt(int fd, int level, int option, void *value, socklen_t *len) {
    (void)fd;
    if (level == SOL_SOCKET && option == SO_ERROR && value && len &&
        *len >= (socklen_t)sizeof(int)) {
        *(int *)value = 0;
        *len = (socklen_t)sizeof(int);
        return 0;
    }
    if (level == SOL_SOCKET && option == SO_TYPE && value && len &&
        *len >= (socklen_t)sizeof(int)) {
        *(int *)value = SOCK_STREAM;
        *len = (socklen_t)sizeof(int);
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;
