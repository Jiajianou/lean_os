/* system_api/include/syscall.h
 *
 * The syscall ABI contract between kernel/ and user_space/ (see
 * system_api/README.md). Both sides include this and nothing else to
 * agree on numbering and calling convention - user_space/lib's wrappers
 * (M10) and kernel/arch/x86_64/syscall.c's dispatch table are built from
 * the same numbers so they can't silently drift apart.
 *
 * Entry mechanism: `int 0x80` (chosen over `syscall`/`sysret` - which
 * needs MSR setup (STAR/LSTAR/FMASK) this project hasn't done - because
 * it reuses the IDT/ISR machinery already built for M4, at the cost of
 * being slower per-call than `syscall`; revisit if that ever matters).
 *
 * Calling convention (System V-flavored, not Linux's `syscall` ABI):
 *   rax = syscall number on entry, return value on exit
 *   rdi, rsi, rdx, rcx, r8, r9 = args 1-6
 * Unlike Linux's `syscall` instruction, `int 0x80` doesn't clobber rcx or
 * r11, so there's no need to shuffle arg 4 into r10 the way the raw
 * `syscall` ABI does - args go in exactly the same registers a normal
 * SysV C call would already use.
 */
#pragma once

#define SYS_write      0
#define SYS_exit       1
#define SYS_getpid     2
#define SYS_spawn      3 /* (path, arg) -> pid or -1. Combines fork+exec into one call - a fork/exec "equivalent" (M13), not literal fork(): no address-space duplication, just a fresh process loaded straight from a named file. */
#define SYS_wait       4 /* (pid) -> exit code. Polls + cooperatively yields (schedule()) rather than a real blocking wait queue - M14 is where "more complete wait semantics" is scoped to land. */
#define SYS_read       5 /* (fd, buf, len) -> bytes read. Only fd=0 (stdin/keyboard) is wired up; blocks (yields) until at least one byte is available. */
#define SYS_readfile   6 /* (name, buf, maxlen) -> bytes copied or -1. Whole-file read by name - no open/close/fd-table/lseek yet, matching M13's "a couple of coreutils" scope rather than a full VFS API nothing needs yet. */
#define SYS_listfiles  7 /* (buf, maxlen) -> bytes written or -1. Newline-separated filenames - leanfs is flat (no directories), so this is the entire namespace. */
#define SYS_kill       8 /* (pid, sig) -> 0 or -1. Only SIGKILL/SIGTERM (signal.h) are recognized; delivery isn't truly asynchronous - it's checked at the next syscall entry or scheduler tick, which is enough for the two signals this project supports (both just terminate). */
#define SYS_pipe       9 /* (fds_out[2]) -> 0 or -1. Installs a read fd and a write fd into the caller's own descriptor table; a child spawned afterward inherits both (SYS_spawn copies the whole fd table). */
#define SYS_getpgid   10 /* (pid) -> pgid or -1. Read-only - nothing needs to *change* a process's group yet (no job control), so there's no setpgid. */

#define SYSCALL_COUNT 11
