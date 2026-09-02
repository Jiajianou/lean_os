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
