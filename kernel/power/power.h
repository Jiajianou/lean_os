/* kernel/power/power.h
 *
 * M47: turning the machine off and restarting it. Every session of this
 * OS before this milestone ended by killing QEMU - there was no shutdown,
 * no restart, and no ACPI S5 path in the kernel at all (acpi.c walked the
 * tables only far enough to find the MADT for SMP).
 *
 * The shape here is tiers, not a single mechanism, because every one of
 * these is firmware-dependent and a machine that will not restart at all
 * is worse than one that restarts inelegantly. Each tier says in the log
 * which one actually fired, so a machine that powers off the ugly way
 * says so instead of looking like it worked properly.
 */
#pragma once

#include <stdint.h>

#include "power_mode.h" /* system_api/include/power_mode.h - POWER_OFF/POWER_REBOOT, shared with SYS_shutdown's callers */

/* Reads the FADT once (acpi.h's acpi_find_power) and remembers it. Called
 * from kernel_main next to acpi_find_madt - doing it at shutdown time
 * would mean walking ACPI tables from inside a path whose whole job is to
 * be as short and as failure-tolerant as possible. */
void power_init(void);

/* M47: SIGTERMs every task but the caller and the idle ones, waits up to
 * `grace_ticks` PIT ticks (drivers/pit.h - a real bounded wait, not a
 * spin count) for them to go, then SIGKILLs whatever is left and waits
 * again. Returns how many tasks had to be SIGKILLed, which is what makes
 * the escalation testable: called with grace_ticks 0, nothing gets a
 * chance to notice the SIGTERM and every survivor dies of the second
 * signal instead - and the two are told apart by the exit code every task
 * already carries (128 + signal).
 *
 * Separated from power_shutdown below precisely so a self-test can drive
 * it without the machine turning off underneath it. */
int power_orderly_stop(uint64_t grace_ticks);

/* The whole sequence: orderly stop, flush the filesystem, then off. Never
 * returns - not even if every tier fails, in which case it halts with a
 * log line rather than returning to a caller that has already had its
 * world torn down. */
void power_shutdown(int mode) __attribute__((noreturn));
