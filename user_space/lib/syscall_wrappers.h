/* user_space/lib/syscall_wrappers.h
 *
 * Thin C wrappers around the syscalls defined in system_api/include/
 * syscall.h - one function per syscall, matching its arguments, hiding
 * the `int 0x80` calling convention. Named "syscall_wrappers.h" rather
 * than the more obvious "syscall.h" so a quoted #include from this
 * file's own .c doesn't resolve to itself instead of the shared ABI
 * header - the exact collision kernel/arch/x86_64/syscall_entry.h was
 * renamed to avoid.
 */
#pragma once

#include <stddef.h>

#include "display.h" /* system_api/include/display.h - display_mode_t, M58 */
#include "os_time.h" /* system_api/include/os_time.h - os_datetime_t/os_stat_t, M59 */
#include "syscall.h" /* system_api/include/syscall.h - M59: the OPEN_ and SEEK_ flags belong to the ABI, not to this wrapper layer */
#include "input.h" /* system_api/include/input.h - mouse_event_t */
#include "power_mode.h" /* system_api/include/power_mode.h - POWER_OFF/POWER_REBOOT, M47 */
#include "proc.h"  /* system_api/include/proc.h - task_info_t, M45 */
#include "wm.h"    /* system_api/include/wm.h - wm_fb_info_t */

/* M52: the raw ABI, with no typed wrapper in the way. Exists for one
 * caller - user_space/bin/badptr.c, which runs this milestone's
 * garbage-argument matrix and therefore has to pass arguments no typed
 * signature would let it express (a `void *` that is really 0xFFFF...,
 * a length of ~0). Nothing else should use it: the typed wrappers below
 * are the interface, and reaching past them in an ordinary program is
 * how an ABI change stops being a compile error. */
long sys_raw(long num, long a1, long a2, long a3);

long sys_write(int fd, const void *buf, size_t len);
void sys_exit(int code) __attribute__((noreturn));
long sys_getpid(void);

/* arg may be NULL for a program that doesn't take one. Returns the new
 * process's pid, or -1 if `path` doesn't exist on disk. */
long sys_spawn(const char *path, const char *arg);

/* M60: the real thing. `argv` is a NULL-terminated array of the arguments
 * *after* the program name - the kernel puts the path in argv[0] itself.
 * sys_spawn above is this with a one-element vector, kept because almost
 * every caller in this project has exactly one thing to say ("open this
 * file") and rewriting them all to build an array would say less. */
long sys_spawnv(const char *path, const char *const *argv);
/* Blocks (cooperatively) until `pid` has terminated; returns its exit
 * code, or -1 if `pid` was never valid. */
long sys_wait(long pid);

/* Only fd=0 (stdin) works. Blocks until at least one byte is available. */
long sys_read(int fd, void *buf, size_t len);

/* Whole-file read by name. Returns the file's size (may exceed maxlen,
 * in which case only maxlen bytes were actually copied) or -1 if it
 * doesn't exist. */
long sys_readfile(const char *name, void *buf, size_t maxlen);

/* Newline-separated filenames into buf. Returns bytes written, or -1. */
/* M53: was sys_listfiles(buf, maxlen). Lists one directory; every name
 * that is itself a directory comes back with a '/' appended, so a caller
 * knows what entering it would mean without a second call. Returns bytes
 * written, or -1 if `path` is not a directory. */
long sys_listdir(const char *path, void *buf, size_t maxlen);

/* M53: creates one directory whose parent already exists. */
long sys_mkdir(const char *path);

/* M56: removes one regular file. Returns 0 or -1. */
long sys_unlink(const char *path);

/* M56: moves one entry to a new name, which may be in another directory.
 * Refuses a destination that already exists. Returns 0 or -1. */
long sys_rename(const char *old_path, const char *new_path);

/* M55: drops a mapping without freeing what it points at - see
 * SYS_shm_unmap. For a client whose compositor died holding its window
 * buffer's frames. */
long sys_shm_unmap(void *vaddr, unsigned long bytes);

/* Only SIGKILL/SIGTERM (system_api/include/signal.h) are recognized.
 * Returns 0, or -1 if pid doesn't name a live task. */
long sys_kill(long pid, int sig);

/* Installs a pipe's read/write ends into the caller's own fd table and
 * writes their fd numbers to fds_out[2] (fds_out[0] = read end,
 * fds_out[1] = write end) - a child spawned afterward (sys_spawn)
 * inherits both. Returns 0, or -1 on failure (no free fd slots, or out
 * of memory). */
long sys_pipe(int fds_out[2]);

long sys_getpgid(long pid);

/* M19: grows this process's heap by `increment` bytes (must be >= 0 -
 * this sbrk is growth-only) and returns the previous break, or -1 on
 * failure. Backs malloc.h - programs that just want to allocate memory
 * should use that instead of calling this directly. */
long sys_sbrk(long increment);

/* M19: allocates a kernel-owned shared-memory segment of `size` bytes,
 * not yet mapped into anyone's address space. Returns its id (>= 0), or
 * -1 on failure. */
long sys_shm_create(size_t size);

/* M19: maps shm segment `id` (created by this process or handed to it -
 * e.g. via the single-string argv mechanism SYS_spawn already supports -
 * by a different one) into this process's own address space. Returns the
 * mapped virtual address, or -1 on failure (unknown id). */
long sys_shm_map(long id);

/* M20: fills *out with the real display's geometry. Returns 0, or -1 if
 * out is NULL. */
long sys_fb_info(wm_fb_info_t *out);

/* M20: maps the real linear framebuffer into this process's own address
 * space (by convention, only the compositor should call this). Returns
 * the mapped virtual address, or -1 on failure. */
long sys_fb_map(void);

/* M58: the display's *offered* modes - a curated, already-validated list
 * (see kernel/drivers/dispi.h), not an enumeration. Copies at most `max`
 * entries into `out` and returns how many exist, which may be more than
 * `max` and is zero on any machine whose adapter has no runtime
 * mode-setting interface. */
long sys_display_modes(display_mode_t *out, long max);

/* M58: changes the display resolution, now, with no reboot. Returns 0, or
 * -1 for a geometry that is not one sys_display_modes offered. Changes
 * the mode and the kernel's own framebuffer mapping and nothing else -
 * every consequence of "the screen is a different size" belongs to the
 * process that owns the screen. */
long sys_display_set_mode(uint32_t width, uint32_t height);

/* ---- M59: files with descriptors -----------------------------------
 *
 * sys_readfile/sys_writefile are still the right call for a small whole
 * file and every existing caller is one. These are for the three things
 * a whole-file API cannot do: read a file bigger than a buffer, write
 * part of one, and hand a file to a child as its stdout. */
long sys_open(const char *path, uint32_t flags);
long sys_lseek(int fd, long offset, int whence);
long sys_stat(const char *path, os_stat_t *out);
long sys_rmdir(const char *path);

/* M59: what time it is. Fills *out (may be NULL) and returns seconds
 * since 1970 - or 0 on a machine with no readable clock, which is also
 * what an un-timestamped file carries, so "no clock" and "no timestamp"
 * print the same way. */
long sys_time(os_datetime_t *out);

/* M20: pops the next buffered mouse event into *out. Returns 1 if one
 * was available, 0 if not (never blocks) - same contract as the
 * kernel-internal mouse_read() this wraps. */
long sys_mouse_read(mouse_event_t *out);

/* M20: like sys_pipe, but rendezvous by name (kernel/ipc/pipe.h's
 * pipe_named) instead of only via an inherited fd - the mechanism two
 * unrelated processes (e.g. a compositor and a client the shell launched
 * separately) use to find each other. Returns 0, or -1 on failure. */
long sys_pipe_open(const char *name, int fds_out[2]);

/* M21: pops the next raw decoded keystroke into *out. Returns 1 if one
 * was available, 0 if not (never blocks) - same non-blocking contract as
 * sys_mouse_read, for a caller (the compositor) that can't afford to
 * block on any one input source. */
long sys_kbd_read(char *out);

/* M21: bytes currently buffered on pipe read-end `fd`, without consuming
 * them - lets a caller check whether sys_read would return immediately
 * before calling it. Returns -1 if fd isn't a pipe read end. */
long sys_pipe_poll(int fd);

/* M21: milliseconds since the timer was initialized - for time-driven
 * redraws that have nothing to do with input arriving. */
long sys_uptime_ms(void);

/* Duplicates oldfd's fd-table slot into newfd (overwriting whatever was
 * there - no SYS_close exists to release it first). Returns newfd, or -1
 * if either fd is out of range or oldfd isn't open. Lets a caller point
 * its own fd 1 at a pipe write end before SYS_spawn, so the child it
 * spawns inherits that pipe as its stdout instead of the global console. */
long sys_dup2(int oldfd, int newfd);

/* Non-blocking counterpart to sys_wait: returns pid's exit code (and
 * reaps it) if it has already terminated, -2 if it's still running, or
 * -1 if pid is invalid. For a caller (a GUI terminal) that can't afford
 * to block on sys_wait while it still has window events to service. */
long sys_wait_nb(long pid);

/* Voluntarily gives up the rest of this task's current time slice right
 * away instead of spinning through it - for a non-blocking poller (no
 * event/work found this pass) that would otherwise busy-spin doing
 * nothing until the next PIT tick preempts it. See SYS_yield's comment
 * in system_api/include/syscall.h. */
long sys_yield(void);

/* M29: non-reaping liveness check - 1 if pid is still READY/RUNNING, 2 if
 * it exited on purpose (SYS_exit(0) - an ordinary completion, not a
 * crash), 0 if it terminated with a nonzero exit code (signal death or
 * any other SYS_exit - treated as "crashed"), -1 if pid never named a
 * task. Unlike sys_wait_nb, safe to call on a task you didn't spawn
 * (doesn't touch its `reaped` flag) - for a caller (the compositor) that
 * merely wants to notice a connected client died *unexpectedly*, not
 * reap it as a child or tear down a window that finished drawing and
 * exited cleanly on its own terms. */
long sys_task_alive(long pid);

/* M29: clears a named pipe's buffered bytes and closed flags back to
 * empty, in place - for reclaiming a pipe (e.g. a crashed GUI client's
 * event pipe) for reuse by whatever connects next, so leftover
 * undelivered bytes can't be misdelivered. fd must be either end of a
 * pipe. Returns 0, or -1 if fd isn't a pipe fd. */
long sys_pipe_reset(int fd);

/* M32: live held/not-held Ctrl/Alt/Shift state (system_api/include/
 * input.h's KBD_MOD_* bits) - not the buffered ASCII stream SYS_kbd_read/
 * SYS_read already deliver, which can't distinguish a plain 'c' from a
 * Ctrl+C chord. */
long sys_kbd_modifiers(void);

/* M32: the single kernel-owned global clipboard (kernel/ipc/clipboard.h).
 * sys_clipboard_set replaces its contents (truncated to CLIPBOARD_MAX,
 * not an error); sys_clipboard_get copies up to maxlen bytes into buf and
 * returns the clipboard's real length (may exceed maxlen - caller's
 * responsibility to size its buffer, same contract as sys_readfile). */
long sys_clipboard_set(const void *buf, size_t len);
long sys_clipboard_get(void *buf, size_t maxlen);

/* M33: creates or overwrites a whole file by name (the write half of
 * sys_readfile). Returns 0, or -1 on failure (name too long, no free
 * inode, no free space, or len exceeds leanfs's max file size). */
long sys_writefile(const char *name, const void *buf, size_t len);

/* M45: fills up to max_entries task_info_t records (system_api/include/
 * proc.h) with a snapshot of every task the scheduler has ever created,
 * terminated ones included. Returns how many were written, or -1 if buf
 * is NULL or max_entries is 0. The caller sizes the array - same
 * contract as sys_listfiles. */
long sys_taskinfo(task_info_t *buf, long max_entries);

/* M47: stops the machine - POWER_OFF or POWER_REBOOT
 * (system_api/include/power_mode.h). Does not return on success; returns
 * -1 for a mode this kernel doesn't recognize. Everything running is
 * SIGTERMed, given a bounded grace period and then SIGKILLed first, so
 * this is a shutdown rather than a power cut. */
long sys_shutdown(int mode);

/* M50: releases this process's own fd-table slot `fd`. Returns 0, or -1
 * for an out-of-range fd or one that wasn't open.
 *
 * Slot only - it does not free or close the underlying pipe, because
 * pipes here are not reference-counted and a named pipe is meant to
 * outlive the fds pointing at it (system_api/include/syscall.h's
 * SYS_close has the full contract). What it fixes is the thing that was
 * actually leaking: a long-lived process burning two slots of MAX_FDS on
 * every reconnect. */
long sys_close(int fd);

/* M50: releases shm segment `id`, which this process must have created,
 * after unmapping it from this process's own address space starting at
 * `vaddr` (what sys_shm_map returned). Returns 0, or -1 for an unknown
 * id, one this process doesn't own, or a `vaddr` that isn't page
 * aligned. Pass vaddr 0 to free a segment this process created but never
 * mapped. See SYS_shm_free in system_api/include/syscall.h for why the
 * address is the caller's to supply. */
long sys_shm_free(long id, void *vaddr);
