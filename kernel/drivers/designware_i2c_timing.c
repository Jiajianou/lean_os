#include "designware_i2c_timing.h"

void designware_i2c_counts(uint32_t clock_khz, uint16_t *high_count, uint16_t *low_count) {
    uint32_t high = (clock_khz * 900u + 500000u) / 1000000u;
    uint32_t low = (clock_khz * 1600u + 500000u) / 1000000u;
    *high_count = (uint16_t)((high > 3u) ? (high - 3u) : 1u);
    *low_count = (uint16_t)((low > 1u) ? (low - 1u) : 1u);
}
