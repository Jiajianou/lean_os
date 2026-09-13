#pragma once

#include <netinet/in.h>
#include <byteswap.h>

#ifdef __cplusplus
extern "C" {
#endif

#define htons(x) ((uint16_t)bswap_16((uint16_t)(x)))
#define ntohs(x) ((uint16_t)bswap_16((uint16_t)(x)))
#define htonl(x) ((uint32_t)bswap_32((uint32_t)(x)))
#define ntohl(x) ((uint32_t)bswap_32((uint32_t)(x)))

in_addr_t inet_addr(const char *s);
int       inet_aton(const char *s, struct in_addr *out);
char     *inet_ntoa(struct in_addr addr);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         inet_pton(int af, const char *src, void *dst);

#ifdef __cplusplus
}
#endif
