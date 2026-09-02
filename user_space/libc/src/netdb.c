/* user_space/libc/src/netdb.c - M89
 *
 * Address conversion and name resolution: the <arpa/inet.h> family,
 * getaddrinfo over M73's DNS client, and the two if_* calls.
 *
 * See <netdb.h> for why there is no /etc/hosts and no /etc/services,
 * and what stands in for them.
 */
#include <netdb.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dns.h"    /* user_space/lib/dns.h - M73's resolver */
#include "syscall_wrappers.h"

int h_errno;

/* ---- dotted quad both ways -------------------------------------------- */

int inet_aton(const char *s, struct in_addr *out) {
    if (!s) {
        return 0;
    }
    uint32_t parts[4];
    int n = 0;
    const char *p = s;
    for (; n < 4; n++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
        uint32_t v = 0;
        int digits = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (uint32_t)(*p++ - '0');
            if (++digits > 3 || v > 255) {
                return 0; /* 256 and 0300 are both rejected - see the note below */
            }
        }
        parts[n] = v;
        if (n < 3) {
            if (*p != '.') {
                return 0;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return 0;
    }
    /* Four decimal parts only. The historical forms - "10.1" meaning
     * 10.0.0.1, and octal or hex parts - are deliberately not accepted:
     * they are a source of address-parsing confusion between programs
     * that agree on nothing else, and nothing here needs them. */
    if (out) {
        out->s_addr = htonl((parts[0] << 24) | (parts[1] << 16) |
                            (parts[2] << 8) | parts[3]);
    }
    return 1;
}

in_addr_t inet_addr(const char *s) {
    struct in_addr a;
    if (!inet_aton(s, &a)) {
        return INADDR_NONE;
    }
    return a.s_addr;
}

char *inet_ntoa(struct in_addr addr) {
    /* A static buffer, which is this function's documented and
     * unfortunate contract - the next call overwrites it. inet_ntop
     * exists because of exactly this and is what new code should use. */
    static char buf[INET_ADDRSTRLEN];
    uint32_t h = ntohl(addr.s_addr);
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (unsigned)(h >> 24) & 0xFF, (unsigned)(h >> 16) & 0xFF,
             (unsigned)(h >> 8) & 0xFF, (unsigned)h & 0xFF);
    return buf;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET || !src || !dst) {
        errno = EAFNOSUPPORT;
        return (const char *)0;
    }
    uint32_t h = ntohl(((const struct in_addr *)src)->s_addr);
    char buf[INET_ADDRSTRLEN];
    int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                     (unsigned)(h >> 24) & 0xFF, (unsigned)(h >> 16) & 0xFF,
                     (unsigned)(h >> 8) & 0xFF, (unsigned)h & 0xFF);
    if (n < 0 || (socklen_t)n >= size) {
        errno = ENOSPC;
        return (const char *)0;
    }
    memcpy(dst, buf, (size_t)n + 1);
    return dst;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton(src, (struct in_addr *)dst) ? 1 : 0;
}

/* ---- services, without /etc/services ---------------------------------
 *
 * A short table rather than a file. <netdb.h> says why: a program asking
 * to connect to "http" should reach port 80, and failing for want of a
 * file nobody here would have written is a worse answer than a table
 * that covers what anything on this machine will ask for. A name not in
 * it is EAI_SERVICE, which is the honest "I do not know that one". */
static int service_port(const char *name, int *out) {
    static const struct { const char *name; int port; } table[] = {
        {"echo", 7}, {"ftp", 21}, {"ssh", 22}, {"telnet", 23},
        {"smtp", 25}, {"domain", 53}, {"http", 80}, {"pop3", 110},
        {"ntp", 123}, {"imap", 143}, {"https", 443}, {"submission", 587},
    };
    if (!name || !*name) {
        *out = 0;
        return 0;
    }
    if (isdigit((unsigned char)*name)) {
        *out = atoi(name);
        return 0;
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(table[i].name, name) == 0) {
            *out = table[i].port;
            return 0;
        }
    }
    return EAI_SERVICE;
}

/* ---- getaddrinfo ------------------------------------------------------ */

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) {
        return EAI_SYSTEM;
    }
    *res = (struct addrinfo *)0;

    int port = 0;
    int e = service_port(service, &port);
    if (e) {
        return e;
    }

    int family = hints ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET) {
        return EAI_FAMILY; /* no IPv6 in this stack - see <sys/socket.h> */
    }
    int socktype = hints ? hints->ai_socktype : 0;
    int flags = hints ? hints->ai_flags : 0;

    uint32_t ip;
    if (!node) {
        /* A NULL node with AI_PASSIVE means "bind to anything", which on
         * a machine with one interface is that interface. */
        ip = (flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK;
    } else {
        struct in_addr a;
        if (inet_aton(node, &a)) {
            ip = ntohl(a.s_addr);
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME; /* the caller said not to resolve, and it is not numeric */
        } else if (dns_resolve(node, &ip) != 0) {
            return EAI_NONAME;
        }
    }

    /* One result. A real getaddrinfo returns a list because a name may
     * have several addresses and several socket types; this machine has
     * one address family and the DNS client reports one address, so a
     * list of one is the whole truth rather than a simplification. */
    struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
    if (!ai) {
        return EAI_MEMORY;
    }
    struct sockaddr_in *sa = (struct sockaddr_in *)(void *)(ai + 1);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    sa->sin_addr.s_addr = htonl(ip);
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
    ai->ai_protocol = ai->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    ai->ai_addrlen = (socklen_t)sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr *)(void *)sa;
    ai->ai_next = (struct addrinfo *)0;
    if ((flags & AI_CANONNAME) && node) {
        size_t n = strlen(node) + 1;
        ai->ai_canonname = malloc(n);
        if (ai->ai_canonname) {
            memcpy(ai->ai_canonname, node, n);
        }
    }
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_canonname);
        free(res); /* the sockaddr is in the same allocation - see getaddrinfo */
        res = next;
    }
}

const char *gai_strerror(int errcode) {
    switch (errcode) {
    case 0:            return "success";
    case EAI_BADFLAGS: return "invalid flags";
    case EAI_NONAME:   return "name or service not known";
    case EAI_AGAIN:    return "temporary failure in name resolution";
    case EAI_FAIL:     return "non-recoverable failure in name resolution";
    case EAI_FAMILY:   return "address family not supported";
    case EAI_SOCKTYPE: return "socket type not supported";
    case EAI_SERVICE:  return "service not known";
    case EAI_MEMORY:   return "out of memory";
    case EAI_SYSTEM:   return "system error";
    default:           return "unknown error";
    }
}

int getnameinfo(const struct sockaddr *addr, socklen_t addrlen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)flags;
    if (!addr || addrlen < (socklen_t)sizeof(struct sockaddr_in)) {
        return EAI_FAMILY;
    }
    const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)addr;
    if (host && hostlen) {
        /* Numeric always. There is no reverse DNS here - M73's client
         * resolves names to addresses and not back - so a caller that
         * did not pass NI_NUMERICHOST gets the number anyway rather than
         * an error, which is what every resolver does when a reverse
         * lookup fails. */
        if (!inet_ntop(AF_INET, &in->sin_addr, host, hostlen)) {
            return EAI_MEMORY;
        }
    }
    if (serv && servlen) {
        snprintf(serv, servlen, "%u", (unsigned)ntohs(in->sin_port));
    }
    return 0;
}

/* ---- gethostbyname, the 1983 spelling --------------------------------- */

struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static struct in_addr addr;
    static char *addr_list[2];
    static char *aliases[1];
    static char namebuf[256];

    uint32_t ip;
    if (!name) {
        h_errno = HOST_NOT_FOUND;
        return (struct hostent *)0;
    }
    struct in_addr a;
    if (inet_aton(name, &a)) {
        ip = ntohl(a.s_addr);
    } else if (dns_resolve(name, &ip) != 0) {
        h_errno = HOST_NOT_FOUND;
        return (struct hostent *)0;
    }
    snprintf(namebuf, sizeof(namebuf), "%s", name);
    addr.s_addr = htonl(ip);
    addr_list[0] = (char *)&addr;
    addr_list[1] = (char *)0;
    aliases[0] = (char *)0;
    he.h_name = namebuf;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = (int)sizeof(struct in_addr);
    he.h_addr_list = addr_list;
    return &he; /* static storage, overwritten by the next call - the documented contract */
}

/* ---- interfaces, of which there is one -------------------------------- */

unsigned int if_nametoindex(const char *name) {
    /* One interface, index 1. Any name is accepted for it, because this
     * machine has never had a second one to tell it apart from and a
     * program that guessed "eth0" should not fail for guessing. */
    (void)name;
    return 1;
}

char *if_indextoname(unsigned int index, char *name) {
    if (index != 1 || !name) {
        errno = ENXIO;
        return (char *)0;
    }
    snprintf(name, IFNAMSIZ, "eth0");
    return name;
}
