/* user_space/libc/include/arpa/inet.h - M89
 *
 * Address conversion, and the byte-order helpers.
 *
 * `htons` and friends are macros over the compiler's byte-swap
 * builtins rather than functions, because they are used in constant
 * expressions - a program writes `htons(80)` in an initialiser and
 * expects it to fold.
 */
#pragma once

#include <netinet/in.h>
#include <byteswap.h>

/* This target is little-endian (see <endian.h>), so host-to-network is a
 * swap in both directions. */
#define htons(x) ((uint16_t)bswap_16((uint16_t)(x)))
#define ntohs(x) ((uint16_t)bswap_16((uint16_t)(x)))
#define htonl(x) ((uint32_t)bswap_32((uint32_t)(x)))
#define ntohl(x) ((uint32_t)bswap_32((uint32_t)(x)))

/* Dotted quad to binary and back. `inet_addr` returns INADDR_NONE for a
 * malformed address, which is indistinguishable from 255.255.255.255 -
 * that ambiguity is in the function's 1983 signature, not in this
 * implementation, and `inet_aton` exists because of it. */
in_addr_t inet_addr(const char *s);
int       inet_aton(const char *s, struct in_addr *out);
char     *inet_ntoa(struct in_addr addr);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int         inet_pton(int af, const char *src, void *dst);
