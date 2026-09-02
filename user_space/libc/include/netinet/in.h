/* user_space/libc/include/netinet/in.h - M89
 *
 * IPv4 addresses in the shape a ported program expects.
 *
 * The one thing to know: `sockaddr_in` holds its address and port in
 * NETWORK byte order, and this kernel's own ABI (system_api/os_net.h)
 * holds them in HOST order - deliberately, and it says why: "this OS
 * never byte-swaps an address into a register, so an on-wire order here
 * would be a second representation to get wrong." Both are right for
 * their side, and the conversion happens in libc's socket.c, which is
 * the single seam between them.
 */
#pragma once

#include <sys/socket.h>
#include <stdint.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr; /* network byte order */
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;   /* network byte order */
    struct in_addr sin_addr;
    char           sin_zero[8];
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_NONE      ((in_addr_t)0xffffffff)

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

/* ---- M89: IPv6, declared and not implemented -------------------------
 *
 * There is no IPv6 in this stack. M27 built Ethernet/ARP/IPv4 and M66
 * built TCP over it; nothing here has ever parsed a 40-byte header or
 * done neighbour discovery, and this milestone does not add one.
 *
 * These declarations exist because a program that has been written for
 * both families puts a `sockaddr_in6` inside a union next to a
 * `sockaddr_in` and cannot compile without the type - toybox's
 * `lib/lib.h` does exactly that, and its `sizeof` decides how big the
 * union is. A type with no stack behind it is a strictly different thing
 * from a call that pretends to work: nothing here returns a success for
 * an AF_INET6 socket. `socket(AF_INET6, ...)` fails with
 * EAFNOSUPPORT, which is precisely the error a machine with no IPv6 is
 * supposed to give and the one every dual-stack program already handles
 * by falling back.
 *
 * The layout is the standard one, byte for byte, so that the day a v6
 * stack exists this is not a second representation to reconcile.
 */
struct in6_addr {
    union {
        uint8_t  __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
};
#define s6_addr   __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;     /* network byte order */
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#define IN6ADDR_ANY_INIT      {{{0}}}
#define IN6ADDR_LOOPBACK_INIT {{{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}}}

#define IPPROTO_IPV6 41
#define IPV6_V6ONLY  26
