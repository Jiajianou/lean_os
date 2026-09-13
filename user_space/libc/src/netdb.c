#include <netdb.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dns.h"
#include "syscall_wrappers.h"

int h_errno;

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
                return 0;
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

char *inet_ntoa(struct in_addr address) {
    static char buffer[INET_ADDRSTRLEN];
    uint32_t h = ntohl(address.s_addr);
    snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u",
             (unsigned)(h >> 24) & 0xFF, (unsigned)(h >> 16) & 0xFF,
             (unsigned)(h >> 8) & 0xFF, (unsigned)h & 0xFF);
    return buffer;
}

const char *inet_ntop(int af, const void *source, char *destination, socklen_t size) {
    if (af != AF_INET || !source || !destination) {
        errno = EAFNOSUPPORT;
        return (const char *)0;
    }
    uint32_t h = ntohl(((const struct in_addr *)source)->s_addr);
    char buffer[INET_ADDRSTRLEN];
    int n = snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u",
                     (unsigned)(h >> 24) & 0xFF, (unsigned)(h >> 16) & 0xFF,
                     (unsigned)(h >> 8) & 0xFF, (unsigned)h & 0xFF);
    if (n < 0 || (socklen_t)n >= size) {
        errno = ENOSPC;
        return (const char *)0;
    }
    memcpy(destination, buffer, (size_t)n + 1);
    return destination;
}

int inet_pton(int af, const char *source, void *destination) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton(source, (struct in_addr *)destination) ? 1 : 0;
}

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
        return EAI_FAMILY;
    }
    int socktype = hints ? hints->ai_socktype : 0;
    int flags = hints ? hints->ai_flags : 0;

    uint32_t ip;
    if (!node) {
        ip = (flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK;
    } else {
        struct in_addr a;
        if (inet_aton(node, &a)) {
            ip = ntohl(a.s_addr);
        } else if (!strcmp(node, "localhost") || !strcmp(node, "localhost.localdomain")) {
            ip = INADDR_LOOPBACK;
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else if (dns_resolve(node, &ip) != 0) {
            return EAI_NONAME;
        }
    }

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
        free(res);
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

int getnameinfo(const struct sockaddr *address, socklen_t addrlen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)flags;
    if (!address || addrlen < (socklen_t)sizeof(struct sockaddr_in)) {
        return EAI_FAMILY;
    }
    const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)address;
    if (host && hostlen) {
        if (!inet_ntop(AF_INET, &in->sin_addr, host, hostlen)) {
            return EAI_MEMORY;
        }
    }
    if (serv && servlen) {
        snprintf(serv, servlen, "%u", (unsigned)ntohs(in->sin_port));
    }
    return 0;
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static struct in_addr address;
    static char *address_list[2];
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
    address.s_addr = htonl(ip);
    address_list[0] = (char *)&address;
    address_list[1] = (char *)0;
    aliases[0] = (char *)0;
    he.h_name = namebuf;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = (int)sizeof(struct in_addr);
    he.h_addr_list = address_list;
    return &he;
}

unsigned int if_nametoindex(const char *name) {
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

const char *hstrerror(int error) {
    switch (error) {
    case 0:              return "Resolver Error 0 (no error)";
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN:      return "Host name lookup failure";
    case NO_RECOVERY:    return "Unknown server error";
    case NO_DATA:        return "No address associated with name";
    default:             return "Unknown resolver error";
    }
}
