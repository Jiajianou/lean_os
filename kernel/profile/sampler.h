/* kernel/profile/sampler.h
 *
 * M101: a sampling profiler, and the reason this arc starts with one.
 *
 * Every milestone after this one proposes to make something faster - a
 * disk cache, per-CPU run queues, a different trap instruction - and
 * M69's rule says performance work on an unmeasured path doesn't get
 * done. Nothing in this tree could attribute a second of wall-clock to
 * code before this file existed: the boot self-tests time *named spans*
 * a person chose in advance, which answers "did this get slower" and
 * cannot answer "where does the time go".
 *
 * ---- What this samples, and what it deliberately does not -------------
 *
 * On every PIT tick (100 Hz - see PIT_HZ), the interrupted RIP goes into
 * a hash table keyed by (rip, pid). That is a **flat** profile: which
 * instruction was executing, how often. There is no caller attribution.
 *
 * The frame-pointer chain this milestone originally asked for was
 * dropped, and the reason is worth stating because it is a measurement
 * decision rather than a scoping one. Walking a chain needs
 * `-fno-omit-frame-pointer` on every kernel translation unit, which
 * changes the code generation of every path this project has a budget
 * for - including the five in tests/budgets.tsv. Buying caller
 * attribution by perturbing every existing measurement, before any
 * measurement has asked for caller attribution, is precisely the trade
 * M69 exists to refuse. The flat profile answers this arc's question
 * ("which function is the bootstrap in") and the day one needs to know
 * *who called it*, that is a milestone with a number behind it.
 *
 * ---- Why the histogram is in the kernel and the symbols are not -------
 *
 * The kernel stores raw addresses. `/bin/profile` resolves them against
 * /etc/kernel.syms, which the build generates from the linked ELF.
 *
 * The alternative - a symbol table compiled into the kernel, Linux's
 * kallsyms - has a bootstrap problem this project does not need: adding
 * the table changes the addresses the table describes, so it takes two
 * link passes and a fixed-point iteration to converge. Keeping symbols
 * in a file has none of that, costs the kernel nothing, and puts string
 * handling in the one place where a bug is not a panic.
 *
 * ---- What 100 Hz buys, stated rather than implied ---------------------
 *
 * One sample every 10 ms per CPU. A ten-second workload gives ~1000
 * samples per core, which is enough to rank the top ten functions and
 * not enough to say anything trustworthy about the eleventh. Anything
 * that runs for less than a second should not be profiled with this.
 * The tick is the machine's timebase and raising it would change every
 * scheduler quantum on the machine; M103 puts a per-CPU LAPIC timer in,
 * which is the honest place for a faster sampling clock to come from.
 */
#pragma once

#include <stdint.h>

#include "arch/x86_64/isr.h"
#include "profile.h" /* system_api/include/profile.h - prof_sample_t,
                      * prof_stats_t, PROF_PID_KERNEL.
                      *
                      * This file is sampler.h and not profile.h for
                      * exactly this line. A quoted include searches the
                      * including file's own directory first, so a
                      * kernel/profile/profile.h that tried to include
                      * the ABI's profile.h would find *itself*, and
                      * #pragma once would make that an empty include
                      * with the types still undefined - a failure whose
                      * error message is about a type rather than about
                      * an include path. The types live in system_api
                      * because they cross the syscall boundary, and Q3's
                      * rule is that a struct with two declarations has
                      * two layouts eventually. */

/* Open-addressed, power of two, and sized against what 100 Hz can
 * actually produce: a sixty-second profile on eight cores is 48,000
 * samples, and a workload with more than 2048 *distinct* hot addresses
 * in it does not have a top ten. Overflow is counted and reported
 * rather than silently dropped - see prof_stats_t.overflow. */
#define PROF_BUCKETS 2048

/* Zeroes the table and the counters. Safe while running. */
void profile_reset(void);

/* Sampling is off until something asks for it, and that is deliberate:
 * a profiler that is always on is a tax on every interrupt the machine
 * takes, paid by every workload, to answer a question nobody asked. */
void profile_start(void);
void profile_stop(void);

/* Called from the timer interrupt with the frame the CPU pushed. Cheap
 * and lock-held-briefly by construction: one hash, a short probe, one
 * increment. Does nothing at all when stopped. */
void profile_sample(isr_regs_t *regs);

void profile_get_stats(prof_stats_t *out);

/* Copies up to `max` occupied buckets out. Densest first is NOT
 * promised - the caller sorts. Returns how many were written.
 *
 * Sorting in user space rather than here is the same call the symbol
 * table made: a sort over 2048 entries in an interrupt-adjacent kernel
 * path buys nothing that /bin/profile cannot do with a real stack and
 * no lock held. */
int profile_snapshot(prof_sample_t *out, int max);
