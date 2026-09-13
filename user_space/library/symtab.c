#include "symtab.h"

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int symtab_parse(symtab_t *out, symtab_entry_t *storage, int capacity,
                 const char *text, size_t len) {
    if (!out || !storage || capacity <= 0 || !text) {
        return -1;
    }
    out->entries = storage;
    out->capacity = capacity;
    out->count = 0;

    size_t i = 0;
    while (i < len && out->count < capacity) {
        size_t line_start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t line_end = i;
        if (i < len) {
            i++;
        }

        size_t p = line_start;
        uint64_t addr = 0;
        int digits = 0;
        while (p < line_end) {
            int v = hex_value(text[p]);
            if (v < 0) {
                break;
            }
            if (digits >= 16) {
                digits = 0;
                break;
            }
            addr = (addr << 4) | (uint64_t)v;
            digits++;
            p++;
        }
        if (digits == 0) {
            continue;
        }
        while (p < line_end && (text[p] == ' ' || text[p] == '\t')) {
            p++;
        }
        if (p >= line_end) {
            continue;
        }

        symtab_entry_t *e = &out->entries[out->count++];
        e->addr = addr;
        e->name = &text[p];
        e->name_len = (uint32_t)(line_end - p);
    }
    return out->count;
}

int symtab_is_sorted(const symtab_t *st) {
    if (!st || st->count <= 1) {
        return 1;
    }
    for (int i = 1; i < st->count; i++) {
        if (st->entries[i].addr < st->entries[i - 1].addr) {
            return 0;
        }
    }
    return 1;
}

const symtab_entry_t *symtab_lookup(const symtab_t *st, uint64_t addr) {
    if (!st || st->count == 0) {
        return (const symtab_entry_t *)0;
    }
    if (addr < st->entries[0].addr) {
        return (const symtab_entry_t *)0;
    }
    if (addr >= st->entries[st->count - 1].addr) {
        return (const symtab_entry_t *)0;
    }

    int lo = 0;
    int hi = st->count - 1;
    while (lo + 1 < hi) {
        int mid = lo + (hi - lo) / 2;
        if (st->entries[mid].addr <= addr) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return &st->entries[lo];
}
