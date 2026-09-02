/* user_space/libc/include/endian.h - M89
 *
 * Byte order, stated rather than detected.
 *
 * This machine is x86-64 and x86-64 is little-endian; there is no
 * configuration under which that is not true here, and the constants
 * below say so. They are the compiler's own (`__ORDER_LITTLE_ENDIAN__`)
 * rather than numbers written out, so a program comparing __BYTE_ORDER
 * against __LITTLE_ENDIAN is comparing two things GCC defined.
 *
 * The htobe/htole family is the useful part: it is what a program
 * writing a file format or a packet header calls, and getting it from a
 * header means the calls compile rather than each program inventing its
 * own macro.
 */
#pragma once

#include <byteswap.h>

/* M97: C++ linkage.
 *
 * Without this every declaration below is a C++ function when a C++
 * program includes it, so `malloc` in a header and `malloc` in libc.a
 * are different symbols and nothing links. It cost a whole libstdc++
 * build to find, and the error names the caller rather than the header:
 * "undefined reference to `malloc(unsigned long)`" - with the argument
 * list, which is the tell. */
#ifdef __cplusplus
extern "C" {
#endif

#define __LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#define __BIG_ENDIAN    __ORDER_BIG_ENDIAN__
#define __PDP_ENDIAN    __ORDER_PDP_ENDIAN__
#define __BYTE_ORDER    __BYTE_ORDER__

#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER

/* Little-endian host, so the "to little" direction is free and the "to
 * big" direction is a swap. Written as the general form anyway - the
 * preprocessor picks, and a big-endian target would get the other half
 * without this file being edited. */
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htobe16(x) bswap_16(x)
#define htobe32(x) bswap_32(x)
#define htobe64(x) bswap_64(x)
#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#else
#define htobe16(x) ((uint16_t)(x))
#define htobe32(x) ((uint32_t)(x))
#define htobe64(x) ((uint64_t)(x))
#define htole16(x) bswap_16(x)
#define htole32(x) bswap_32(x)
#define htole64(x) bswap_64(x)
#endif

/* The reverse direction is the same operation. */
#define be16toh(x) htobe16(x)
#define be32toh(x) htobe32(x)
#define be64toh(x) htobe64(x)
#define le16toh(x) htole16(x)
#define le32toh(x) htole32(x)
#define le64toh(x) htole64(x)

#ifdef __cplusplus
}
#endif
