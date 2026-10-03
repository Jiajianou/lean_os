#pragma once

#include <sys/socket.h>
#include <stdint.h>
#include <byteswap.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
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

/* The names Linux gives the same option levels. A program that sets
   TCP_NODELAY spells it one way or the other and means the same thing. */
#define SOL_IP       IPPROTO_IP
#define SOL_TCP      IPPROTO_TCP
#define SOL_UDP      IPPROTO_UDP

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

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
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

/* The address-class tests RFC 2553 defines. They are written on the bytes
   rather than on 32-bit words so that they need no byte-order conversion
   and therefore no other header. The argument is a pointer to an
   in6_addr. */
#define __in6_byte(a, i) (((const unsigned char *)(a))[i])
#define __in6_zero12(a)                                                   \
    (__in6_byte(a, 0) == 0 && __in6_byte(a, 1) == 0 &&                    \
     __in6_byte(a, 2) == 0 && __in6_byte(a, 3) == 0 &&                    \
     __in6_byte(a, 4) == 0 && __in6_byte(a, 5) == 0 &&                    \
     __in6_byte(a, 6) == 0 && __in6_byte(a, 7) == 0 &&                    \
     __in6_byte(a, 8) == 0 && __in6_byte(a, 9) == 0 &&                    \
     __in6_byte(a, 10) == 0 && __in6_byte(a, 11) == 0)

#define IN6_IS_ADDR_UNSPECIFIED(a)                                        \
    (__in6_zero12(a) && __in6_byte(a, 12) == 0 && __in6_byte(a, 13) == 0 &&\
     __in6_byte(a, 14) == 0 && __in6_byte(a, 15) == 0)
#define IN6_IS_ADDR_LOOPBACK(a)                                           \
    (__in6_zero12(a) && __in6_byte(a, 12) == 0 && __in6_byte(a, 13) == 0 &&\
     __in6_byte(a, 14) == 0 && __in6_byte(a, 15) == 1)
#define IN6_IS_ADDR_MULTICAST(a) (__in6_byte(a, 0) == 0xFF)
#define __in6_mc_scope(a, s)                                              \
    (IN6_IS_ADDR_MULTICAST(a) && (__in6_byte(a, 1) & 0x0F) == (s))
#define IN6_IS_ADDR_MC_NODELOCAL(a) __in6_mc_scope(a, 0x1)
#define IN6_IS_ADDR_MC_LINKLOCAL(a) __in6_mc_scope(a, 0x2)
#define IN6_IS_ADDR_MC_SITELOCAL(a) __in6_mc_scope(a, 0x5)
#define IN6_IS_ADDR_MC_ORGLOCAL(a)  __in6_mc_scope(a, 0x8)
#define IN6_IS_ADDR_MC_GLOBAL(a)    __in6_mc_scope(a, 0xE)
#define IN6_IS_ADDR_LINKLOCAL(a)                                          \
    (__in6_byte(a, 0) == 0xFE && (__in6_byte(a, 1) & 0xC0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a)                                          \
    (__in6_byte(a, 0) == 0xFE && (__in6_byte(a, 1) & 0xC0) == 0xC0)
#define IN6_IS_ADDR_V4MAPPED(a)                                           \
    (__in6_byte(a, 0) == 0 && __in6_byte(a, 1) == 0 &&                    \
     __in6_byte(a, 2) == 0 && __in6_byte(a, 3) == 0 &&                    \
     __in6_byte(a, 4) == 0 && __in6_byte(a, 5) == 0 &&                    \
     __in6_byte(a, 6) == 0 && __in6_byte(a, 7) == 0 &&                    \
     __in6_byte(a, 8) == 0 && __in6_byte(a, 9) == 0 &&                    \
     __in6_byte(a, 10) == 0xFF && __in6_byte(a, 11) == 0xFF)
#define IN6_IS_ADDR_V4COMPAT(a)                                           \
    (__in6_zero12(a) &&                                                   \
     !(__in6_byte(a, 12) == 0 && __in6_byte(a, 13) == 0 &&                \
       __in6_byte(a, 14) == 0 && __in6_byte(a, 15) <= 1))
#define IN6_ARE_ADDR_EQUAL(a, b)                                          \
    (((const struct in6_addr *)(a))->s6_addr32[0] ==                      \
         ((const struct in6_addr *)(b))->s6_addr32[0] &&                  \
     ((const struct in6_addr *)(a))->s6_addr32[1] ==                      \
         ((const struct in6_addr *)(b))->s6_addr32[1] &&                  \
     ((const struct in6_addr *)(a))->s6_addr32[2] ==                      \
         ((const struct in6_addr *)(b))->s6_addr32[2] &&                  \
     ((const struct in6_addr *)(a))->s6_addr32[3] ==                      \
         ((const struct in6_addr *)(b))->s6_addr32[3])

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#define IN6ADDR_ANY_INIT      {{{0}}}
#define IN6ADDR_LOOPBACK_INIT {{{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}}}

#define IPPROTO_IPV6 41
#define IPV6_V6ONLY  26

#define IP_TTL             2
#define IP_MULTICAST_IF    32
#define IP_MULTICAST_TTL   33
#define IP_MULTICAST_LOOP  34
#define IP_ADD_MEMBERSHIP  35
#define IP_DROP_MEMBERSHIP 36

#define IPV6_MULTICAST_LOOP 19
#define IPV6_ADD_MEMBERSHIP 20
#define IPV6_DROP_MEMBERSHIP 21
#define IPV6_JOIN_GROUP     IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP    IPV6_DROP_MEMBERSHIP

/* The rest of the IP-level option numbers, at the values Linux gives them.
   A number here is a name a program can spell, not a promise that
   setsockopt(2) will accept it - this kernel refuses the ones it does not
   implement, which is the truthful answer and the one M65 asks for. */
/* glibc declares these here as well as in <arpa/inet.h>, and network code
   includes whichever of the two it happens to need. The definitions are
   the same tokens, so a translation unit that gets both is fine. */
#ifndef htons
#define htons(x) ((uint16_t)bswap_16((uint16_t)(x)))
#define ntohs(x) ((uint16_t)bswap_16((uint16_t)(x)))
#define htonl(x) ((uint32_t)bswap_32((uint32_t)(x)))
#define ntohl(x) ((uint32_t)bswap_32((uint32_t)(x)))
#endif

#define IP_DEFAULT_MULTICAST_TTL  1
#define IP_DEFAULT_MULTICAST_LOOP 1
#define IP_MAX_MEMBERSHIPS        20

#define IP_TOS             1
#define IP_HDRINCL         3
#define IP_OPTIONS         4
#define IP_PKTINFO         8
#define IP_RECVTOS         13
#define IP_RECVTTL         12
#define IP_RECVERR         11
#define IP_MTU_DISCOVER    10
#define IP_MTU             14
#define IP_UNBLOCK_SOURCE  37
#define IP_BLOCK_SOURCE    38
#define IP_ADD_SOURCE_MEMBERSHIP  39
#define IP_DROP_SOURCE_MEMBERSHIP 40

#define IP_PMTUDISC_DONT   0
#define IP_PMTUDISC_WANT   1
#define IP_PMTUDISC_DO     2
#define IP_PMTUDISC_PROBE  3

#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_PKTINFO        50
#define IPV6_HOPLIMIT       52
#define IPV6_RECVERR        25
#define IPV6_RECVPKTINFO    49
#define IPV6_RECVHOPLIMIT   51
#define IPV6_MTU_DISCOVER   23
#define IPV6_MTU            24
#define IPV6_TCLASS         67
#define IPV6_RECVTCLASS     66
#define IPV6_DONTFRAG       62

#define IPV6_PMTUDISC_DONT  0
#define IPV6_PMTUDISC_WANT  1
#define IPV6_PMTUDISC_DO    2
#define IPV6_PMTUDISC_PROBE 3

/* RFC 3678's protocol-independent multicast source filters. */
#define MCAST_JOIN_GROUP         42
#define MCAST_BLOCK_SOURCE       43
#define MCAST_UNBLOCK_SOURCE     44
#define MCAST_LEAVE_GROUP        45
#define MCAST_JOIN_SOURCE_GROUP  46
#define MCAST_LEAVE_SOURCE_GROUP 47

#define MCAST_INCLUDE 1
#define MCAST_EXCLUDE 0

/* Linux's third multicast-join shape, which names the interface by index
   rather than by address. */
struct ip_mreqn {
    struct in_addr imr_multiaddr;
    struct in_addr imr_address;
    int            imr_ifindex;
};

struct group_req {
    uint32_t                gr_interface;
    struct sockaddr_storage gr_group;
};

struct group_source_req {
    uint32_t                gsr_interface;
    struct sockaddr_storage gsr_group;
    struct sockaddr_storage gsr_source;
};

struct in_pktinfo {
    int            ipi_ifindex;
    struct in_addr ipi_spec_dst;
    struct in_addr ipi_addr;
};

struct in6_pktinfo {
    struct in6_addr ipi6_addr;
    unsigned int    ipi6_ifindex;
};

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

struct ip_mreq_source {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
    struct in_addr imr_sourceaddr;
};

struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int    ipv6mr_interface;
};

#ifdef __cplusplus
}
#endif
