#pragma once

#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};

#define h_addr h_addr_list[0]

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x400
#define AI_ADDRCONFIG  0x20
#define AI_ALL         0x10
#define AI_V4MAPPED    0x08

#define EAI_BADFLAGS  -1
#define EAI_NONAME    -2
#define EAI_AGAIN     -3
#define EAI_FAIL      -4
#define EAI_FAMILY    -6
#define EAI_SOCKTYPE  -7
#define EAI_SERVICE   -8
#define EAI_MEMORY    -10
#define EAI_SYSTEM    -11
#define EAI_OVERFLOW  -12
/* Not in POSIX, and glibc's own value for it. Programs that tell "no such
   name" apart from "that name has no address of the family you asked for"
   test for this one by name. */
#define EAI_NODATA    -5
#define EAI_ADDRFAMILY -9

#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NOFQDN      4
#define NI_NAMEREQD    8
#define NI_DGRAM       16
#define NI_NUMERICSCOPE 0x100
#define NI_MAXHOST     256
#define NI_MAXSERV     32

#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4

extern int h_errno;

const char *hstrerror(int err);

int  getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int  getnameinfo(const struct sockaddr *addr, socklen_t addrlen,
                 char *host, socklen_t hostlen,
                 char *serv, socklen_t servlen, int flags);

struct hostent *gethostbyname(const char *name);

/* The services database is /etc/services in its usual format. A machine
   without one has no names for ports, and these say so by returning null
   rather than by carrying a table of their own. */
struct servent {
    char  *s_name;
    char **s_aliases;
    int    s_port;
    char  *s_proto;
};

struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);
int getservbyname_r(const char *name, const char *proto, struct servent *result,
                    char *buffer, size_t length, struct servent **out);
int getservbyport_r(int port, const char *proto, struct servent *result,
                    char *buffer, size_t length, struct servent **out);

#ifdef __cplusplus
}
#endif
