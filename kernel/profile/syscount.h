/* kernel/profile/syscount.h
 *
 * M101: how many times each syscall was called, and how long they took.
 *
 * The flat profile beside this (profile.h) says which *instruction* the
 * machine was executing. This says which *call* it was executing on
 * behalf of, which is the other half of attributing a workload: a
 * bootstrap that spends a third of its time in `leanfs_handle_read` is a
 * different problem depending on whether that is ten reads or ten
 * million.
 *
 * There are SYSCALL_COUNT entries in syscall_table and, before this
 * file, no idea which of them a real workload calls at all. That is not
 * a small gap: every one of them was covered by whichever milestone
 * added it, and none of that says anything about frequency.
 *
 * ---- The two halves have different costs, so they have different
 *      switches -----------------------------------------------------
 *
 * **Counting is always on.** One `lock xadd` per syscall, on a table
 * indexed by syscall number. Roughly twenty cycles against a trap path
 * that is thousands, and the number is useful on every boot rather than
 * only when somebody remembered to ask.
 *
 * **Timing is off until asked for.** Two serialised TSC reads bracket
 * the dispatch, and `lfence; rdtsc` is not free - on the order of thirty
 * cycles each, which is a measurable tax on the shortest syscalls
 * (SYS_getpid does almost nothing else). A measurement that changes the
 * thing it measures should be opt-in, and this one is: syscount_timing.
 *
 * ---- Why atomics rather than a per-CPU table ---------------------------
 *
 * The obvious lock-free shape is one row per CPU, summed on readout, and
 * it was not taken because getting "which CPU am I" here costs more than
 * the atomic does. `smp_current_cpu()` reads the Local APIC's ID
 * register over MMIO and then scans the CPU table - which this kernel
 * already does on every `sched_current()` call, and which is the first
 * thing this milestone's own profile found. Adding a second one per
 * syscall to avoid a `lock xadd` would be paying more to save less.
 * See M101's notes; the fix belongs in the scheduler, not here.
 */
#pragma once

#include <stdint.h>

#include "syscall.h" /* system_api/include/syscall.h - SYSCALL_COUNT */

typedef struct {
    uint64_t calls;
    uint64_t cycles; /* 0 unless timing was on for those calls */
} syscount_entry_t;

/* Records one dispatch. `cycles` is 0 when timing is off. */
void syscount_record(int num, uint64_t cycles);

/* Timing costs two serialised TSC reads per syscall - see the header
 * comment. Returns the previous setting so a caller can restore it. */
int syscount_set_timing(int on);
int syscount_timing_enabled(void);

void syscount_reset(void);

/* Reads one entry. Out-of-range numbers return zeroes rather than
 * failing: this is an instrument, and a reader walking 0..SYSCALL_COUNT
 * should not have to know which numbers are holes in the table. */
void syscount_get(int num, syscount_entry_t *out);

/* Total calls across every syscall number, for a report that wants to
 * express one line as a percentage of the whole. */
uint64_t syscount_total_calls(void);
