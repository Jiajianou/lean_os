#pragma once

#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPVERSION    4
#define IP_MAXPACKET 65535

#define IPTOS_TOS_MASK    0x1E
#define IPTOS_TOS(tos)    ((tos) & IPTOS_TOS_MASK)
#define IPTOS_LOWDELAY    0x10
#define IPTOS_THROUGHPUT  0x08
#define IPTOS_RELIABILITY 0x04
#define IPTOS_MINCOST     0x02

#define IPTOS_DSCP_MASK   0xFC
#define IPTOS_DSCP(x)     ((x) & IPTOS_DSCP_MASK)
#define IPTOS_ECN_MASK    0x03
#define IPTOS_ECN(x)      ((x) & IPTOS_ECN_MASK)
#define IPTOS_ECN_NOT_ECT 0x00
#define IPTOS_ECN_ECT1    0x01
#define IPTOS_ECN_ECT0    0x02
#define IPTOS_ECN_CE      0x03

#define IPTOS_PREC_MASK            0xE0
#define IPTOS_PREC(tos)            ((tos) & IPTOS_PREC_MASK)
#define IPTOS_PREC_NETCONTROL      0xE0
#define IPTOS_PREC_INTERNETCONTROL 0xC0
#define IPTOS_PREC_CRITIC_ECP      0xA0
#define IPTOS_PREC_FLASHOVERRIDE   0x80
#define IPTOS_PREC_FLASH           0x60
#define IPTOS_PREC_IMMEDIATE       0x40
#define IPTOS_PREC_PRIORITY        0x20
#define IPTOS_PREC_ROUTINE         0x00

struct iphdr {
    uint8_t  ihl : 4;
    uint8_t  version : 4;
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};

#ifdef __cplusplus
}
#endif
