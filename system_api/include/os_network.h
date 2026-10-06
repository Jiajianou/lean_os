#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint16_t reserved;
} os_sockaddr_t;

#define OS_SOCKET_DGRAM  0
#define OS_SOCKET_STREAM 1

#define OS_AF_INET 0
#define OS_AF_UNIX 1

/* M226: what a socket is, asked of the kernel - its family, its type, and
   for an AF_UNIX socket the name bound to it (or to its peer). getsockname
   used to describe every socket as an IP address, so libuv took a Unix
   socket a pipe had passed it for TCP. TYPE is OS_SOCKET_* for AF_INET and
   the type the AF_UNIX socket was made with otherwise; NAME_LENGTH is zero
   for an unnamed socket, and an abstract name keeps its leading zero byte. */
typedef struct {
    uint32_t family;
    uint32_t type;
    uint32_t name_length;
    uint32_t reserved;
    char name[108];
} os_socket_identity_t;

/* SO_PEERCRED. The pid is the thread group at the other end of a connected
   AF_UNIX socket; uid and gid are this machine's single principal, which is
   what M65 says a one-principal machine reports. */
typedef struct {
    int32_t  pid;
    uint32_t uid;
    uint32_t gid;
} os_ucred_t;

typedef struct {
    uint64_t data;
    uint32_t length;
    uint32_t nfds;
    uint64_t file_descriptors;
    uint32_t flags;
    uint32_t reserved;
} os_message_t;

#define OS_MESSAGE_MAX_FILE_DESCRIPTORS 8

#define OS_MESSAGE_TRUNC  1
#define OS_MESSAGE_CTRUNC 2

typedef struct {
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
    uint32_t dns;
    int32_t leased;
} os_netconf_t;

#define OS_IPV4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static inline char *os_ip_to_string(uint32_t ip, char *out) {
    int n = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint32_t octet = (ip >> shift) & 0xFF;
        if (octet >= 100) { out[n++] = (char)('0' + octet / 100); }
        if (octet >= 10)  { out[n++] = (char)('0' + (octet / 10) % 10); }
        out[n++] = (char)('0' + octet % 10);
        if (shift) { out[n++] = '.'; }
    }
    out[n] = '\0';
    return out;
}

static inline int os_ip_from_string(const char *s, uint32_t *out) {
    uint32_t ip = 0;
    for (int octet = 0; octet < 4; octet++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (++digits > 3 || v > 255) {
                return 0;
            }
        }
        ip = (ip << 8) | v;
        if (octet < 3) {
            if (*s != '.') {
                return 0;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return 0;
    }
    *out = ip;
    return 1;
}

#ifdef __cplusplus
}
#endif
