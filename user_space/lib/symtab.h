/* user_space/lib/symtab.h - M101
 *
 * Turning a sampled address back into a function name.
 *
 * The profiler stores raw addresses (kernel/profile/sampler.h says why),
 * and this is the other half of that decision: a parser for the file
 * tools/gen-kernel-syms.sh writes, and a lookup over it.
 *
 * ---- The format it reads ----------------------------------------------
 *
 *   0000000000100000 _start
 *   0000000000100050 enter_user_mode
 *   ...
 *   000000000012d0b5 [end-of-text]
 *
 * Ascending by address, one entry per line, sixteen hex digits then a
 * space then a name. The last line is the end of .text rather than a
 * function, which is what lets a lookup distinguish "in the last
 * function" from "past the end of the kernel" - see symtab_lookup.
 *
 * ---- Why this is its own translation unit -----------------------------
 *
 * Because it is the only part of /bin/profile that can be wrong quietly.
 * A binary search with a bad boundary condition resolves addresses to
 * the function *before* the right one, which produces a report that is
 * entirely plausible and entirely wrong - and no amount of booting the
 * machine would reveal it, because there is nothing to compare against
 * from in there. Split out, it is a pure function over a buffer, and the
 * host tier can hand it the cases a booted machine cannot: an address
 * below the first symbol, an address exactly on a boundary, a truncated
 * file, a line with no name, a file with one entry.
 *
 * That is the Q2 argument applied to user-space code for the first time.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t addr;
    const char *name; /* points into the caller's buffer - not a copy */
    uint32_t name_len;
} symtab_entry_t;

typedef struct {
    symtab_entry_t *entries;
    int count;
    int capacity;
} symtab_t;

/* Parses `text` (length `len`) in place into `out`, filling at most
 * `capacity` entries. The buffer must outlive the symtab: names point
 * into it rather than being copied, which is what keeps this allocation
 * free.
 *
 * Returns the number of entries parsed, or -1 if the arguments are
 * unusable. A malformed line is skipped rather than fatal: a symbol file
 * with one bad line should still resolve the other seven hundred, and
 * refusing the whole file would turn a cosmetic problem into no report
 * at all.
 *
 * Does NOT sort. The file is required to be ascending (the generator
 * emits it that way) and sorting here would hide a generator that
 * stopped doing so - see symtab_is_sorted, which the tool calls and
 * complains about rather than repairing.
 */
int symtab_parse(symtab_t *out, symtab_entry_t *storage, int capacity,
                 const char *text, size_t len);

/* 1 if every entry's address is >= the one before it. */
int symtab_is_sorted(const symtab_t *st);

/* The entry `addr` falls inside: the last one whose address is <= addr.
 *
 * NULL when the table is empty, when `addr` is below the first entry, or
 * when it is at or past the final entry - the final entry being the
 * end-of-text marker rather than a function, so "resolved to the last
 * line" and "past the end of the kernel" are the same thing and both are
 * a NULL rather than a confident wrong answer.
 */
const symtab_entry_t *symtab_lookup(const symtab_t *st, uint64_t addr);
