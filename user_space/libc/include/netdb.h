/* user_space/libc/include/netdb.h - M89
 *
 * Name resolution, over the DNS client M73 built (user_space/lib/dns.c).
 *
 * `getaddrinfo` is the call a modern program makes and the one this
 * header exists for; `gethostbyname` is the 1983 spelling and is here
 * because half the ported code in the world still calls it.
 *
 * There is no /etc/hosts and no /etc/services on this machine. A
 * numeric address resolves without a query; a name goes to the DNS
 * server SYS_netconf reports; a service name (`"http"`) is looked up in
 * a small built-in table, because the alternative is failing to connect
 * to port 80 for want of a file nobody here would have written.
 */
#pragma once

#include <sys/socket.h>
#include <netinet/in.h>

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

#define EAI_BADFLAGS  -1
#define EAI_NONAME    -2
#define EAI_AGAIN     -3
#define EAI_FAIL      -4
#define EAI_FAMILY    -6
#define EAI_SOCKTYPE  -7
#define EAI_SERVICE   -8
#define EAI_MEMORY    -10
#define EAI_SYSTEM    -11

#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_MAXHOST     256
#define NI_MAXSERV     32

#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4

extern int h_errno;

int  getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int  getnameinfo(const struct sockaddr *addr, socklen_t addrlen,
                 char *host, socklen_t hostlen,
                 char *serv, socklen_t servlen, int flags);

struct hostent *gethostbyname(const char *name);
