#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
typedef int bool;
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (sizeof(long) * 8 - 1 - (h))))
#define ETH_ALEN 6
#define IEEE80211_MAX_SSID_LEN 32
#define __force
#define offsetofend(TYPE, MEMBER) (offsetof(TYPE, MEMBER) + sizeof(((TYPE *)0)->MEMBER))
#define DECLARE_FLEX_ARRAY(T, name) T name[]
#define __counted_by(x)
typedef uint64_t dma_addr_t;
/* rs.h carries inline rate converters; the layout check never calls them,
   but they have to compile. The values are wrong on purpose rather than
   pretended right. */
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define u32_encode_bits(v, m) ((u32)(v) & (u32)(m))
#define le32_encode_bits(v, m) ((u32)(v) & (u32)(m))
#define u32_get_bits(v, m) ((u32)(v) & (u32)(m))
#define le32_get_bits(v, m) ((u32)(v) & (u32)(m))
/* tx.h ends its commands with the 802.11 header, as a flexible array. */
struct ieee80211_hdr {
    __le16 frame_control;
    __le16 duration_id;
    u8 addr1[6];
    u8 addr2[6];
    u8 addr3[6];
    __le16 seq_ctrl;
} __packed;
