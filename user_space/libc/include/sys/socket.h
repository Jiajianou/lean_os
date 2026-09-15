#pragma once

#include <sys/types.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_LOCAL  AF_LOCAL
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_SEQPACKET 5

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

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

#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_WAITALL   0x08
#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000
#define MSG_TRUNC     0x20
#define MSG_CTRUNC    0x08

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
#define SO_PEERCRED  17
#define SO_PASSCRED  16

/* The credentials of the process at the other end of a connected AF_UNIX
   socket. This machine has one principal, so uid and gid are always 0 and
   the pid is the only field carrying information - which is the field
   programs that ask for this actually want. */
struct ucred {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

struct linger {
    int l_onoff;
    int l_linger;
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
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

struct cmsghdr {
    size_t cmsg_len;
    int    cmsg_level;
    int    cmsg_type;
};

#define SCM_RIGHTS 1

/* The sender's identity, as a control message rather than a socket option.
   The kernel stamps it; a process cannot claim to be another one, which is
   the whole reason this exists beside SO_PEERCRED. */
#define SCM_CREDENTIALS 2

#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(len) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(len)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_DATA(cmsg) ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))

#define CMSG_FIRSTHDR(mhdr) \
    ((size_t)(mhdr)->msg_controllen >= sizeof(struct cmsghdr) \
         ? (struct cmsghdr *)(mhdr)->msg_control \
         : (struct cmsghdr *)0)

#define CMSG_NXTHDR(mhdr, cmsg)                                                   \
    ((cmsg) == (struct cmsghdr *)0 || (cmsg)->cmsg_len < sizeof(struct cmsghdr)   \
         ? (struct cmsghdr *)0                                                    \
         : ((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) +              \
                    sizeof(struct cmsghdr) >                                      \
                (unsigned char *)(mhdr)->msg_control + (mhdr)->msg_controllen     \
                ? (struct cmsghdr *)0                                             \
                : (struct cmsghdr *)((unsigned char *)(cmsg) +                    \
                                     CMSG_ALIGN((cmsg)->cmsg_len))))

int socketpair(int domain, int type, int protocol, int fds[2]);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int setsockopt(int fd, int level, int option, const void *value, socklen_t len);
int getsockopt(int fd, int level, int option, void *value, socklen_t *len);

#ifdef __cplusplus
}
#endif
