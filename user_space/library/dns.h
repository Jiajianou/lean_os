#pragma once

#include <stdint.h>

#define DNS_MAX_NAME 255

int dns_resolve(const char *name, uint32_t *out);

int dns_parse_response(const uint8_t *message, int length, uint16_t expect_id,
                        const char *expect_name, uint32_t *out);

int dns_build_query(const char *name, uint16_t id, uint8_t *buffer, int cap);

#define DNS_MAX_SERVERS 4

int dns_servers(uint32_t *out, int max);

int dns_parse_resolv_conf(const char *text, int length, uint32_t *out, int max);

void dns_cache_clear(void);
