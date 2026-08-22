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
#define SYS_sbrk      11 /* (increment) -> previous break, or (uint64_t)-1. Classic sbrk semantics but growth-only (negative increment fails) - a free-list allocator built on top never needs to give pages back. Backs user_space/lib/malloc.c (M19). */
#define SYS_shm_create 12 /* (size) -> shm id, or -1. Allocates a kernel-owned physical buffer not yet mapped into anyone's address space - see SYS_shm_map. */
#define SYS_shm_map   13 /* (id) -> mapped virtual address, or (uint64_t)-1. Maps a shm segment (created by this process or a different one - ids are a global namespace, not per-process) into the caller's own address space at a kernel-chosen address. Two processes mapping the same id see the same physical pages - the mechanism M19 exists for (compositor <-> app pixel buffers), since pipes copy data and are far too slow for whole-frame transfers. */
#define SYS_fb_info   14 /* (wm_fb_info_t *out) -> 0 or -1. Geometry only (system_api/include/wm.h) - see SYS_fb_map for actually mapping the pixels in. */
#define SYS_fb_map    15 /* () -> mapped virtual address, or (uint64_t)-1. Maps the real linear framebuffer into the caller's own address space - by convention only the compositor (M20) ever calls this, but nothing enforces that yet (no permission model in this project so far). */
#define SYS_mouse_read 16 /* (mouse_event_t *out) -> 1 (an event was copied out) or 0 (none available). Never blocks - same non-blocking contract as the kernel-internal mouse_read() this wraps (kernel/drivers/mouse.h). */
#define SYS_pipe_open 17 /* (name, fds_out[2]) -> 0 or -1. Like SYS_pipe, but finds-or-creates a *named* pipe (kernel/ipc/pipe.h's pipe_named) instead of a fresh anonymous one - the rendezvous mechanism two unrelated processes need (M20's compositor/client protocol, system_api/include/wm.h), since SYS_pipe's fds only ever reach a child spawned afterward. */
#define SYS_kbd_read  18 /* (char *out) -> 1 (a key was copied out) or 0 (none available). Never blocks - the raw-event counterpart to SYS_read(fd=0)'s line-oriented blocking contract, for a process (M21's compositor) that wants to route individual keystrokes rather than edit a line itself. Wraps the same kernel/drivers/keyboard.h ring buffer SYS_read already drains, so the two aren't meant to be used by two different foreground processes at once - fine for now since nothing in this project runs the compositor and the text shell concurrently (see kernel/kernel.c's M20/M21 self-tests). */
#define SYS_pipe_poll 19 /* (fd) -> bytes currently buffered (>= 0), or -1 if fd isn't a pipe read end. Never blocks or consumes - lets a caller (M21's compositor, juggling multiple named pipes in one loop) check whether a blocking SYS_read would return immediately before calling it. */
#define SYS_uptime_ms 20 /* () -> milliseconds since PIT init (drivers/pit.h's tick counter, M6). Backs time-driven redraws (M21) that have nothing to do with input arriving. */
#define SYS_dup2      21 /* (oldfd, newfd) -> newfd or -1. Duplicates oldfd's fd-table slot into newfd - the missing half of SYS_pipe/SYS_pipe_open, which only ever allocate brand-new fd numbers; a caller that wants an *existing* fd number (its own stdout, fd 1) to become a pipe end instead needs this. No SYS_close exists to release whatever newfd used to hold - newfd's old slot is simply overwritten, fine for now since nothing has ever needed one. */
#define SYS_wait_nb   22 /* (pid) -> exit code (and reaps it) if pid has already terminated, -2 if it's still running, -1 if pid is invalid. Non-blocking counterpart to SYS_wait for a caller (a GUI terminal) that has its own event loop to keep servicing rather than blocking until a spawned child exits. */
#define SYS_yield     23 /* () -> 0. Voluntarily gives up the rest of the caller's current SCHED_QUANTUM_TICKS time slice (kernel/sched/sched.c) right away instead of spinning through it - the non-blocking pollers that make up the whole GUI stack (compositor.c's main loop, wmclient.c's wm_poll_event) have no blocking read to fall back on the way SYS_wait/SYS_read(fd=0) do, so without this every one of them busy-spins for a full 50ms slice even when a poll finds nothing to do, and round-robin makes every *other* runnable task wait out that same 50ms before its own turn comes back around. */

#define SYSCALL_COUNT 24
