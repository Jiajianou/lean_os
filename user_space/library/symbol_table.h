#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t addr;
    const char *name;
    uint32_t name_len;
} symbol_table_entry_t;

typedef struct {
    symbol_table_entry_t *entries;
    int count;
    int capacity;
} symbol_table_t;

int symbol_table_parse(symbol_table_t *out, symbol_table_entry_t *storage, int capacity,
                 const char *text, size_t len);

int symbol_table_is_sorted(const symbol_table_t *st);

const symbol_table_entry_t *symbol_table_lookup(const symbol_table_t *st, uint64_t addr);
