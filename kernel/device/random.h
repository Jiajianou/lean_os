#pragma once

#include <stddef.h>
#include <stdint.h>

void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                    uint32_t counter, uint8_t out[64]);

void random_init(const void *seed, size_t length);

void random_feed(const void *data, size_t length);

void random_bytes(void *out, size_t length);

uint64_t random_events(void);

int random_has_rdrand(void);
int random_has_rdseed(void);
