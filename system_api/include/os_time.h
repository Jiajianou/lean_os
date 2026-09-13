#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t size;
    uint32_t mtime;
    uint8_t is_dir;
    uint8_t is_link;
    uint8_t kind;
    uint32_t inode;
} os_stat_t;

#define OS_STAT_FILE 0
#define OS_STAT_DIR  1
#define OS_STAT_CHR  2
#define OS_STAT_FIFO 3
#define OS_STAT_SOCK 4

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t valid;
} os_datetime_t;

static inline uint32_t os_days_from_civil(uint16_t year, uint8_t month, uint8_t day) {
    uint32_t y = year;
    uint32_t m = month;
    y -= m <= 2;
    uint32_t era = y / 400;
    uint32_t yoe = y - era * 400;
    uint32_t doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097u + doe - 719468u;
}

static inline uint32_t os_unix_time(const os_datetime_t *t) {
    return os_days_from_civil(t->year, t->month, t->day) * 86400u +
           (uint32_t)t->hour * 3600u + (uint32_t)t->minute * 60u + t->second;
}

static inline void os_civil_from_unix(uint32_t secs, os_datetime_t *out) {
    uint32_t days = secs / 86400u;
    uint32_t rem = secs % 86400u;
    out->hour = (uint8_t)(rem / 3600u);
    out->minute = (uint8_t)((rem % 3600u) / 60u);
    out->second = (uint8_t)(rem % 60u);

    uint32_t z = days + 719468u;
    uint32_t era = z / 146097u;
    uint32_t doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t y = yoe + era * 400u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t m = mp + (mp < 10u ? 3u : (uint32_t)-9);
    out->year = (uint16_t)(y + (m <= 2u));
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    out->valid = 1;
}

#ifdef __cplusplus
}
#endif
