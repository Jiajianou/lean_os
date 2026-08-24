/* system_api/include/signal.h
 *
 * M14's "basic set" of signals - just enough for SYS_kill to be
 * meaningful. Numbered to match POSIX for familiarity, though this
 * project only recognizes these two and only supports the default
 * (terminate) action - no handler registration/blocking/masking.
 */
#pragma once

#define SIGTERM 15
#define SIGKILL 9

/* M52: not a signal anything can *send* - SYS_kill still refuses
 * anything but the two above. It exists so that a task the kernel kills
 * for faulting has an exit code that says so, through the same
 * "128 + signal" convention every other death here already uses (139).
 * Numbered to match POSIX like its neighbors, and named rather than
 * spelled 139 at the two places that care (kernel/arch/x86_64/isr.c
 * raises it, the [m52] self-test asserts on it) so neither can drift. */
#define SIGSEGV 11
