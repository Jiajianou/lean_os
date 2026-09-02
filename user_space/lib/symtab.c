#include "symtab.h"

/* Its own hex parser rather than strtoull, because this file is compiled
 * three ways - into /bin/profile against this project's libc, into the
 * host test binary against the host's, and (one day) wherever else wants
 * it - and the one thing all three agree on is what a hex digit is. */
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
        /* One line: hex, one space, name, newline. */
        size_t line_start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t line_end = i;
        if (i < len) {
            i++; /* past the newline */
        }

        size_t p = line_start;
        uint64_t addr = 0;
        int digits = 0;
        while (p < line_end) {
            int v = hex_value(text[p]);
            if (v < 0) {
                break;
            }
            /* A line with more than sixteen hex digits is not an address
             * on this machine, and shifting past 64 bits would wrap into
             * a plausible small number rather than failing. */
            if (digits >= 16) {
                digits = 0;
                break;
            }
            addr = (addr << 4) | (uint64_t)v;
            digits++;
            p++;
        }
        if (digits == 0) {
            continue; /* not an address - a comment, a blank line, junk */
        }
        while (p < line_end && (text[p] == ' ' || text[p] == '\t')) {
            p++;
        }
        if (p >= line_end) {
            continue; /* an address with no name is not an entry */
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
    /* At or past the last entry is unresolvable, and that is the whole
     * reason the generator appends an end-of-text marker. Without it the
     * final *function* would own every address above it, so a wild jump
     * to 0xdeadbeef would be reported as time spent in whichever function
     * the linker happened to place last. */
    if (addr >= st->entries[st->count - 1].addr) {
        return (const symtab_entry_t *)0;
    }

    /* The last entry with addr <= target. Written as a "find first
     * greater, step back" search rather than a classic binary search
     * with an equality case, because the equality case is where this
     * gets written wrong: an address exactly on a symbol's boundary
     * belongs to that symbol, not to the one before it. */
    int lo = 0;
    int hi = st->count - 1; /* the marker, known to be > addr */
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
