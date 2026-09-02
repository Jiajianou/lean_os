/* system_api/include/os_net.h
 *
 * M64: the two structures the network syscalls exchange, and the address
 * helpers a program needs to say one out loud.
 *
 * Addresses are host-order uint32_t everywhere - in the kernel, in this
 * header, and in every program - which is the same decision
 * kernel/net/ip.c made and made for the same reason: this OS only ever
 * runs little-endian, never puts an address in a register the wire
 * format has to match, and byte-swapping at an API boundary is a second
 * representation with a conversion between them to get wrong. There is
 * no htons here and there does not need to be one.
 */
#pragma once

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

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint16_t reserved;
} os_sockaddr_t;

/* M66: what kind of socket SYS_socket should make. DGRAM is 0 so every
 * call written against M64's parameterless version still means what it
 * meant - see SYS_socket's own note on why the parameter appeared only
 * once there was something to choose between. */
#define OS_SOCK_DGRAM  0
#define OS_SOCK_STREAM 1

typedef struct {
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
    uint32_t dns;
    int32_t leased; /* 1 if a DHCP server said so, 0 if these are the fallback constants */
} os_netconf_t;

#define OS_IPV4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Formats an address into `out`, which needs 16 bytes. Returns `out`, so
 * it drops into a printf argument. A static inline for the same reason
 * shortcut_lookup is one: it is four numbers and three dots, and giving
 * it a translation unit both sides would have to link would be more
 * machinery than the thing itself. */
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

/* Parses "a.b.c.d" into `out`. Returns 1 on success, 0 on anything else -
 * including a trailing octet over 255, which is the typo a lenient
 * parser turns into a silently different address. */
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
