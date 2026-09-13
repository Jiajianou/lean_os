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
