#pragma once

#include <stdint.h>

#define NET_FALLBACK_IP      0x0A00020Fu
#define NET_FALLBACK_GATEWAY 0x0A000202u
#define NET_FALLBACK_MASK    0xFFFFFF00u
#define NET_FALLBACK_DNS     0x0A000203u

#define NET_BROADCAST_IP     0xFFFFFFFFu
#define NET_LOOPBACK_NET     0x7F000000u
#define NET_LOOPBACK_MASK    0xFF000000u

int net_init(void);

/* What IP runs over: a wired card found at boot, or the radio once it has
   joined a network. One at a time - this machine has one address and one
   route, and a second link would need a routing table to mean anything. */
typedef struct {
    const char *name;
    uint8_t address[6];
    int (*send)(const uint8_t *frame, uint16_t length);
} net_link_t;

/* Makes `link` the one IP runs over, starting the stack on first use and
   forgetting every neighbour the last link knew. Returns 0 when another link
   already carries IP - a wired card is not displaced by a radio. The caller
   then asks for an address with dhcp_configure(). */
int net_attach_link(const net_link_t *link);
void net_detach_link(const net_link_t *link);
const net_link_t *net_active_link(void);

int net_link_send(const uint8_t *frame, uint16_t length);

void net_log_configuration(void);

const uint8_t *net_local_mac(void);

uint32_t net_local_ip(void);
uint32_t net_gateway_ip(void);
uint32_t net_subnet_mask(void);
uint32_t net_dns_ip(void);

int net_config_is_leased(void);

int net_have_nic(void);

void net_set_config(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns);

int net_is_local_ip(uint32_t ip);

void net_lock_acquire(void);
void net_lock_release(void);
