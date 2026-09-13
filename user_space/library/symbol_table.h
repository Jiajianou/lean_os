#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t addr;
    const char *name;
    uint32_t name_len;
} symtab_entry_t;

typedef struct {
    symtab_entry_t *entries;
    int count;
    int capacity;
} symtab_t;

int symtab_parse(symtab_t *out, symtab_entry_t *storage, int capacity,
                 const char *text, size_t len);

int symtab_is_sorted(const symtab_t *st);

const symtab_entry_t *symtab_lookup(const symtab_t *st, uint64_t addr);
