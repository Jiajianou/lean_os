#pragma once

#include <stdint.h>

#define E820_TYPE_USABLE       1
#define E820_TYPE_RESERVED     2
#define E820_TYPE_ACPI_RECLAIM 3
#define E820_TYPE_ACPI_NVS     4
#define E820_TYPE_MMIO         6

static inline int e820_is_ram(uint32_t type) {
    return type != E820_TYPE_MMIO;
}

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_ext;
} e820_entry_t;

static inline uint32_t e820_count(const uint32_t *map) {
    return *map;
}

static inline const e820_entry_t *e820_entries(const uint32_t *map) {
    return (const e820_entry_t *)((const uint8_t *)map + 8);
}
