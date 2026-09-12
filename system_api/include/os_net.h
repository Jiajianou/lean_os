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

/* ---- M118: which family SYS_socket should make one in -----------------
 *
 * AF_INET is 0 so that every call written against the one-parameter
 * version still means what it meant, the same trick OS_SOCK_DGRAM played
 * in M66 and for the same reason. The numbers are deliberately NOT
 * <sys/socket.h>'s AF_INET (2) and AF_UNIX (1): that header's values are
 * Linux's, this is a two-valued choice in an ABI of this project's own,
 * and a constant that looks like somebody else's while meaning something
 * else is worse than one that clearly does not. libc maps them, at the
 * same seam that already maps byte order. */
#define OS_AF_INET 0
#define OS_AF_UNIX 1

/* ---- M118: a message, which is bytes plus descriptors ----------------
 *
 * What the kernel's sendmsg/recvmsg exchange, and deliberately not
 * <sys/socket.h>'s `struct msghdr`. That structure is an iovec array and
 * a byte blob of cmsghdr records to be walked with CMSG_NXTHDR - a
 * user-space calling convention, with alignment rules, whose whole
 * content is "here are some bytes and some descriptors". Parsing it in
 * the kernel would mean the kernel validating somebody else's
 * convention; libc does it instead, one seam, the same file that already
 * converts sockaddr_in's byte order (user_space/libc/src/socket.c).
 *
 * `fds` points at `nfds` ints: the descriptors to send, or room for the
 * ones that arrive. On return from SYS_recvmsg, `nfds` is how many
 * actually came and `flags` says whether anything was dropped on the way.
 */
typedef struct {
    uint64_t data;  /* user pointer to the payload bytes */
    uint32_t len;   /* how many of them */
    uint32_t nfds;  /* descriptors: how many are at `fds`, and on receive how many arrived */
    uint64_t fds;   /* user pointer to int[nfds] - may be 0 when nfds is 0 */
    uint32_t flags; /* out: OS_MSG_TRUNC / OS_MSG_CTRUNC */
    uint32_t reserved;
} os_msg_t;

/* Descriptors one message can carry, and ONE definition for both sides:
 * kernel/ipc/unixsock.h's UNIX_MAX_FDS is this constant, because a second
 * hand-picked number that merely happened to agree is exactly the
 * near-duplicate cap this project has shipped bugs behind three times
 * (M40, M41, M50).
 *
 * Eight. Linux's SCM_MAX_FD is 253 and Chromium's own
 * base::UnixDomainSocket::kMaxFileDescriptors is 16; Mojo passes a
 * handful. The cost of matching Linux would be 60 KiB of kernel memory
 * per socket for a message nothing sends, and a caller that asks for more
 * is refused rather than silently truncated. */
#define OS_MSG_MAX_FDS 8

/* A SEQPACKET message was longer than the buffer offered, and the rest is
 * gone - POSIX's MSG_TRUNC, which is a report and not an error. */
#define OS_MSG_TRUNC  1
/* Descriptors arrived that did not fit in `nfds`, and they have been
 * closed. MSG_CTRUNC. */
#define OS_MSG_CTRUNC 2

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
