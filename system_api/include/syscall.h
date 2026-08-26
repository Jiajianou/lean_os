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
/* M60: the most arguments SYS_spawn will carry. A vector longer than
 * this is truncated at the last whole argument rather than refused - a
 * program seeing fewer arguments than it was given is a failure it can
 * report, and half an argument names something else. Sixteen is the
 * width of every command line this OS's terminal can produce and then
 * some; the real ceiling is the single page the vector is copied into. */
#define SPAWN_MAX_ARGS 16

#define SYS_spawn      3 /* (path, argv) -> pid or -1 (a SPAWN_ERR_* code, M48). Combines fork+exec into one call - a fork/exec "equivalent" (M13), not literal fork(): no address-space duplication, just a fresh process loaded straight from a named file. M60: `argv` is a NULL-terminated array of char* holding the arguments *after* the program name - the kernel puts `path` in argv[0] itself, because that is the one element it knows for certain and the one a program is entitled to assume is there. NULL is a program launched with no arguments. Truncated at SPAWN_MAX_ARGS, or at whatever fits in the single page the vector is copied into; the one-string form every caller in this project used until now is user_space/lib's sys_spawn(), which builds a two-element vector. */
#define SYS_wait       4 /* (pid) -> exit code. Polls + cooperatively yields (schedule()) rather than a real blocking wait queue - M14 is where "more complete wait semantics" is scoped to land. */
#define SYS_read       5 /* (fd, buf, len) -> bytes read. Only fd=0 (stdin/keyboard) is wired up; blocks (yields) until at least one byte is available. */
#define SYS_readfile   6 /* (name, buf, maxlen) -> bytes copied or -1. Whole-file read by name - no open/close/fd-table/lseek yet, matching M13's "a couple of coreutils" scope rather than a full VFS API nothing needs yet. */
#define SYS_listdir    7 /* (path, buf, maxlen) -> bytes written or -1. Newline-separated names of everything in the directory at `path`, with a '/' appended to each one that is itself a directory so a caller can tell the two apart without a second call. M53: this *was* SYS_listfiles(buf, maxlen), which took no path because leanfs was flat and "the entire namespace" was the only answer it could give. Same number, new signature - one repo, every caller converted in the same commit, and leaving a second call that only ever means "/" would just be a way for the two to drift. */
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
#define SYS_dup2      21 /* (oldfd, newfd) -> newfd or -1. Duplicates oldfd's fd-table slot into newfd - the missing half of SYS_pipe/SYS_pipe_open, which only ever allocate brand-new fd numbers; a caller that wants an *existing* fd number (its own stdout, fd 1) to become a pipe end instead needs this. M50: SYS_close finally exists, but this still doesn't call it - newfd's old slot is simply overwritten, which for dup2's one real use (pointing fd 1 at a pipe before SYS_spawn) is what the caller wants, and silently closing whatever was there would be a second effect nobody asked for. */
#define SYS_wait_nb   22 /* (pid) -> exit code (and reaps it) if pid has already terminated, -2 if it's still running, -1 if pid is invalid. Non-blocking counterpart to SYS_wait for a caller (a GUI terminal) that has its own event loop to keep servicing rather than blocking until a spawned child exits. */
#define SYS_yield     23 /* () -> 0. Voluntarily gives up the rest of the caller's current SCHED_QUANTUM_TICKS time slice (kernel/sched/sched.c) right away instead of spinning through it - the non-blocking pollers that make up the whole GUI stack (compositor.c's main loop, wmclient.c's wm_poll_event) have no blocking read to fall back on the way SYS_wait/SYS_read(fd=0) do, so without this every one of them busy-spins for a full 50ms slice even when a poll finds nothing to do, and round-robin makes every *other* runnable task wait out that same 50ms before its own turn comes back around. */
#define SYS_task_alive 24 /* (pid) -> 1 (still READY/RUNNING), 2 (TERMINATED with exit code 0 - an ordinary, intentional completion), 0 (TERMINATED with a nonzero exit code - a signal death (128+sig) or any other non-zero SYS_exit, treated as "crashed"), or -1 (no such task). M29: a non-reaping liveness peek, unlike SYS_wait/SYS_wait_nb which only make sense for an actual parent (they mutate `reaped`, and reaping someone else's child would break that child's real parent's own later wait). Lets a process that merely *tracks* another one it didn't spawn - the compositor watching a GUI client connected over a named pipe, not a fork/exec child - notice it *unexpectedly* died without disturbing SYS_wait's parent/child bookkeeping. The 0-vs-2 split on the terminated case exists so a client that finishes and exits(0) on purpose (M20's wm_demo self-test: draws one static frame, then returns - no crash, nothing wrong) doesn't get its still-wanted window torn down the instant it completes, the same way a real crash's window should be. */
#define SYS_pipe_reset 25 /* (fd) -> 0 or -1. Clears a pipe's buffered bytes (and its closed flags) back to empty, in place - for a named pipe (kernel/ipc/pipe.h's pipe_named) that outlives any one connection, e.g. the compositor recycling a crashed client's event pipe for the next window that connects at the same slot, so stale undelivered events from the dead client can't be misdelivered to the new one. fd must be either end of a pipe (see SYS_pipe_open); returns -1 for any other fd type. */

#define SYS_kbd_modifiers 26 /* () -> bitmask of KBD_MOD_CTRL/ALT/SHIFT (system_api/include/input.h) for the character SYS_kbd_read/SYS_read most recently returned. M32: lets a caller tell a plain keypress from a modifier chord (compositor.c's Alt+Tab, gui_terminal.c's Ctrl+C/V). M40: was live held/not-held state until a chord shorter than one compositor frame turned out to lose its modifier before anyone asked - see kernel/drivers/keyboard.h. */
#define SYS_clipboard_set 27 /* (buf, len) -> 0. Replaces the single kernel-owned clipboard's contents (kernel/ipc/clipboard.h) - truncated to CLIPBOARD_MAX, not an error. */
#define SYS_clipboard_get 28 /* (buf, maxlen) -> the clipboard's real length (may exceed maxlen, same "caller's responsibility to size its buffer" contract as SYS_readfile), or 0 if never set. */
#define SYS_writefile 29 /* (name, buf, len) -> 0 or -1. M33: the missing write half of SYS_readfile - creates or overwrites a whole file by name (kernel/fs/vfs.h's vfs_write, already used internally since M12/M15 but never exposed to user space until text_editor.c needed to save one). Whole-file, no append/seek, same scope-trimming as SYS_readfile's own "no open/close/lseek yet" contract. */

#define SYS_taskinfo 30 /* (task_info_t *buf, max_entries) -> number of entries written, or -1. M45: a whole-shot, read-only snapshot of every task the scheduler has ever created (slots are never recycled, so a terminated task is still listed - with its exit code - exactly as SYS_wait already relies on), taken under the scheduler's own lock. Same "caller sizes the buffer, kernel fills what fits" contract SYS_listfiles has for files: no iterator, no handle, nothing to leak. See system_api/include/proc.h for the record layout, and for why this isn't filtered by owner. */

#define SYS_shutdown 31 /* (mode) -> never returns on success, -1 for an unrecognized mode. M47: mode is system_api/include/power_mode.h's POWER_OFF or POWER_REBOOT. Performs an *orderly* stop rather than just writing a port - SIGTERM every task, a bounded ~1s grace period counted in scheduler ticks, SIGKILL whatever is left, flush the filesystem - and only then hands over to kernel/power/power.h's tiered ACPI/8042/triple-fault machinery. Deliberately not restricted to any caller: this project has no permission model, and pretending otherwise here would be the same fake check the task manager's own desktop-process guard is careful not to make. */

#define SYS_close 32 /* (fd) -> 0, or -1 for an out-of-range fd or one that was already closed. M50: there has never been one - SYS_dup2's own comment admits it, and M40's root-cause bug was five never-released pipe fd-pairs riding into every process on the system. Releases the caller's own fd-table slot and nothing more: pipes are not reference-counted (kernel/ipc/pipe.h), so closing one end does NOT free the pipe object or affect any other holder of it, and a named pipe deliberately outlives every fd that ever pointed at it. That is the honest contract for what this kernel actually has - a general close() would need refcounting nothing here has asked for. */

#define SYS_shm_free 33 /* (id, vaddr) -> 0 or -1. M50: the missing counterpart to SYS_shm_create/SYS_shm_map, without which every window this OS ever composited permanently consumed one of MAX_SHM_SEGMENTS's 32 slots - so roughly thirty window opens exhausted the table for the life of the machine, and the thirty-first silently got no window. Unmaps `id`'s pages from the caller's own address space starting at `vaddr` (which the caller got from SYS_shm_map and is the only one that knows - nothing here tracks who mapped what) and then frees the frames, in that order, so the frames can never be handed to somebody else while this process still has them mapped. Only the segment's creator may free it. */

#define SYS_mkdir 34 /* (path) -> 0 or -1. M53: creates one directory whose parent already exists. Deliberately not recursive ("mkdir -p" is a shell convenience, not a kernel one) and deliberately an error rather than a no-op when the path is already taken, so "I made this" and "this was already here" cannot be confused by a caller that cares. */

#define SYS_shm_unmap 35 /* (vaddr, bytes) -> 0 or -1. M55: removes a mapping from the caller's own address space *without* freeing the frames behind it - the half of SYS_shm_free that makes sense when the segment is somebody else's and, in the case this exists for, no longer exists at all. A client that survives a compositor crash has its window's pixel buffer still mapped, pointing at frames the kernel handed back the moment the compositor died (shm_free_by_owner); leaving that mapping in place would alias whatever those frames become next. Deliberately does not consult the segment table: the whole point is that there may be nothing left to consult. Bounded to the caller's own shm window (proc.h's USER_SHM_BASE..USER_FB_BASE), so it can unmap a window buffer and nothing else - not its code, not its stack, not the framebuffer. */

#define SYS_unlink 36 /* (path) -> 0 or -1. M56: the write half of this filesystem stopped at "create or overwrite a whole file", which is how a filesystem that has never had to *remove* anything ends up with the boot self-tests' own fixtures on it forever. Regular files only - a directory is refused rather than recursed into or emptiness-checked, because rmdir is a different operation with a different failure mode and nothing has asked for one. */
#define SYS_rename 37 /* (old_path, new_path) -> 0 or -1. M56: moves one entry from one name to another, possibly across directories. No data moves - a rename is a change to *records*, which is only true because M53 stopped storing a name in the inode. Refuses a destination that already exists: silently replacing a file is a way to lose one, and the caller can ask. */

/* ---- M59: files with descriptors ------------------------------------
 *
 * SYS_readfile's own comment has carried "no open/close/fd-table/lseek
 * yet" since M13. The fd table was never the missing part - MAX_FDS is
 * 128 and pipes and stdio have lived in it since M14 - what was missing
 * was a *file* entry in it.
 *
 * SYS_readfile/SYS_writefile stay, and stay the recommended call for a
 * small whole file: every current caller is one, and rewriting nine
 * working callers would be scope nothing has asked for. This makes them
 * no longer the only option. */
#define OPEN_READ     0x1
#define OPEN_WRITE    0x2
#define OPEN_CREATE   0x4  /* create if absent - an error otherwise */
#define OPEN_TRUNCATE 0x8  /* drop existing contents (requires OPEN_WRITE) */
#define OPEN_APPEND   0x10 /* start positioned at the end */

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define SYS_open   40 /* (path, flags) -> fd, or -1. Regular files only; a directory is refused rather than opened, because there is nothing a read of one would honestly return that SYS_listdir does not already give. The offset lives in a kernel-side open-file entry shared by every fd SYS_dup2 makes from this one, so two descriptors on the same file advance one position between them. */
#define SYS_lseek  41 /* (fd, offset, whence) -> the new absolute position, or -1. offset is signed. Seeking past the end is allowed and creates a hole on the next write, which reads back as zeros - the ordinary sparse-file behaviour, and cheaper than refusing. -1 for a pipe: a pipe has no position, and saying so beats pretending. */
#define SYS_stat   42 /* (path, os_stat_t *out) -> 0 or -1. Size, mtime and whether it is a directory - the three things the file manager's columns are made of, and what every "how big a buffer do I need" caller in this kernel was previously answering with LEANFS_MAX_FILE_SIZE. */
#define SYS_rmdir  43 /* (path) -> 0 or -1. M59: the same gap SYS_unlink closed for files in M56, left open there because nothing had asked. Empty directories only - recursive delete is one keystroke away from losing everything under a path, and this OS has no trash to take it back out of. */
#define SYS_time   44 /* (os_datetime_t *out, may be NULL) -> seconds since 1970, or 0 on a machine with no readable CMOS clock. The first thing in this project that can answer "what time is it" rather than "how long has this been switched on". */

/* ---- M62: sound -----------------------------------------------------
 *
 * The speaker is *owned* rather than exposed to everything, and that is
 * the same judgment M56 made about keyboard_inject: there is one speaker,
 * and a program able to seize it unasked can make the machine unusable.
 *
 * Ownership is enforceable without a permission model, which is why it is
 * done this way rather than by a comment asking nicely: the first process
 * to claim audio holds it until it exits, and every call below refuses
 * anybody else. The compositor claims it at startup, for the same reason
 * it owns the screen - it is the process that knows when the desktop has
 * something to say.
 */
#define SYS_audio_claim  45 /* () -> 0 if the caller now owns the audio devices, -1 if another live process does. Claiming again from the owner is a no-op success. Released when the owner exits, so a compositor that crashes does not take the machine's sound with it. */
#define SYS_beep         46 /* (freq_hz, ms) -> 0, or -1 if the caller does not own audio. Starts a square wave on the PC speaker and returns immediately - the timer tick turns it off. Works on machines with no sound device at all, which is the whole reason it is here alongside a real one. Silent while muted (see SYS_audio_volume). */
#define SYS_audio_volume 47 /* (percent 0-100) -> 0 or -1. Applied to the AC'97 mixer *and* to the speaker, where the only thing it can honestly do is mute: the speaker is one bit, and a "quiet beep" is not something that hardware can produce. */
#define SYS_audio_release 49 /* () -> 0, or -1 from a process that does not own audio. The counterpart to SYS_audio_claim, and it exists because ownership that is only released by *exiting* is ownership a long-lived process cannot hand over - kernel_main is exactly that: it claims audio for its own boot self-test, never exits, and without this would silently keep the speaker away from the compositor for the life of the machine. Found by writing that test, not by reading the design. */
#define SYS_audio_play   48 /* (const int16_t *samples, frames) -> 0, or -1 with no device, a buffer too large, or from a non-owner. 16-bit signed stereo at 48 kHz - the one format AC'97 is guaranteed to do. Non-blocking: it returns once the device is armed, because a play that blocked would be a sound that stops the compositor drawing. */

#define SYS_display_modes 38 /* (display_mode_t *out, max_entries) -> how many modes exist (may exceed max_entries; same "caller sizes the buffer" contract as SYS_taskinfo), or -1. M58: a *curated and validated* list, not an enumeration - kernel/drivers/dispi.h's own comment explains why there is nothing to enumerate. Zero modes is the honest answer on any machine without a Bochs/QEMU DISPI adapter, which is every real one. */
#define SYS_display_set_mode 39 /* (width, height) -> 0 or -1. M58: reprograms the display adapter and re-maps the kernel's framebuffer, right now, with no reboot - see kernel/drivers/dispi.h for why this has to be a native driver rather than a call back into UEFI GOP. Refuses any geometry not in SYS_display_modes' list. Everything downstream of "the screen is a different size now" is the caller's problem and is deliberately not attempted here: the compositor owns the screen, so the compositor reallocates window buffers, re-spans the panels, clamps windows and the cursor back on-screen, and tells its clients. Not restricted to any caller, for the same reason SYS_shutdown isn't. */

/* M64: the network, reachable at last.
 *
 * M27 shipped Ethernet, ARP, IPv4 and ICMP and then nothing used any of
 * it for thirty-six milestones, because there was no way for a program
 * to ask. These five calls are that way, and they are shaped like the
 * fd table rather than like a new namespace: a socket is an fd, it is
 * inherited across spawn, it is closed by SYS_close, and it costs the
 * kernel one refcounted table entry - all of which M59's open-file work
 * had already built and none of which had to be built again.
 *
 * UDP only. TCP is its own milestone and says so in milestones.md: an
 * eleven-state machine with retransmission and congestion control is not
 * a bullet on somebody else's list.
 */
#define SYS_socket   50 /* (type) -> an fd for a new unbound socket, or -1 if the table is full. os_net.h's OS_SOCK_DGRAM (0) or OS_SOCK_STREAM (1). M64 shipped this with no parameters at all and argued for that in writing: "three parameters that only ever take one value each are three ways to be wrong about an API that has no choices in it". That argument was right for as long as its premise held, and M66's TCP ends it - there is a real choice now, so there is one real parameter and not three. OS_SOCK_DGRAM is 0, so every call written before M66 still means what it meant. */
#define SYS_bind     51 /* (fd, port) -> the bound port, or -1 if the port is taken, the fd is not a socket, or it is already bound. Port 0 asks for an ephemeral one and returns which - so a client that only sends still gets a source port replies can come back to, without having to invent a number and hope. */
#define SYS_sendto   52 /* (fd, ip, port, const void *data, len) -> bytes sent, or -1. `ip` is host-order (10.0.2.2 is 0x0A000202), the same convention kernel/net/net.h uses and for the same reason: this OS never byte-swaps an address into a register, so an on-wire order here would be a second representation to get wrong. Binds an ephemeral source port if the socket has none. An unreachable destination is -1, not a panic - see kernel/net/ip.h on why that had to change. -1 is also the honest answer for the very first datagram to a neighbour nobody has ARPed yet: it is dropped while the request goes out, exactly as it is on any BSD-derived stack, and a caller that cares sends again. */
#define SYS_recvfrom 53 /* (fd, void *data, max, os_sockaddr_t *from) -> bytes copied, or -1 if nothing is queued. Never blocks. A datagram larger than `max` is truncated and the rest discarded, which is what UDP recvfrom does everywhere; `from` (may be NULL) says who sent it, which is the entire difference between this and a read. */
#define SYS_sockpoll 54 /* (fd) -> how many datagrams are queued, or -1. The counterpart to SYS_pipe_poll and the reason a non-blocking recvfrom is usable: it turns "nothing yet" and "nothing ever" into two different answers a caller can wait on with a deadline. */
#define SYS_netconf  55 /* (os_netconf_t *out) -> 0, or -1 with no NIC. This machine's address, mask, gateway and DNS server, and whether they came from a DHCP lease or from the fallback constants. A program that wants to reach a name server needs to be told where one is, and hardcoding 10.0.2.3 in user space would put back exactly the fiction M64 removed from the kernel. */
#define SYS_settime  56 /* (seconds since 1970) -> 0, or -1 for an implausible time. The write half of SYS_time, and it exists because SNTP without it is a program that knows the time and cannot say so. Sets the kernel's own offset rather than the CMOS RTC: writing the hardware clock is a thing a boot-time utility does with the machine's consent, and nothing here has that consent to give. */

/* M66: TCP.
 *
 * A stream socket is the same fd as a datagram one and closes the same
 * way - but `close` on a connected stream is an *active close*, not a
 * discard: the peer gets the FIN and whatever is still queued, and the
 * kernel runs the connection down through FIN_WAIT and TIME_WAIT after
 * the descriptor is gone.
 *
 * Everything here is non-blocking, including connect. A blocking connect
 * would need the wait-queue machinery this kernel has never had, and
 * building one for TCP alone would be a scheduler change hiding inside a
 * networking milestone - so `connect` starts the handshake and
 * `SYS_connstat` says how it went, which is the same poll-with-a-deadline
 * shape every other network call in this OS has.
 */
#define SYS_listen   59 /* (fd) -> 0 or -1. Binds an ephemeral port first if the socket has none, so a server that forgot to bind gets a working socket on a port it can then ask for rather than an error. */
#define SYS_connect  60 /* (fd, ip, port) -> 0 once the SYN is away, or -1 if it could not be sent. NOT "connected" - poll SYS_connstat. Gives up after ten seconds or six retransmissions, whichever comes first, so a program that forgets its own deadline still gets an answer. */
#define SYS_connstat 61 /* (fd) -> 1 established, 0 still trying, -1 failed (refused, unreachable, or timed out). The three answers a caller can act on, out of eleven states it has no business knowing about. */
#define SYS_accept   62 /* (fd, os_sockaddr_t *from) -> a new fd for a completed connection, or -1 if none is waiting. Never blocks; SYS_sockpoll on a listening socket says how many are ready. `from` (may be NULL) is who connected. */
#define SYS_send     63 /* (fd, buf, len) -> bytes queued, which may be less than len and may be 0 when the send buffer is full. Partial writes are the honest answer for a non-blocking send, and a caller loops. -1 once the connection is gone. */
#define SYS_recv     64 /* (fd, buf, max) -> bytes copied, 0 if none are waiting right now, or -1 at end of stream - the peer closed and the buffer is drained, or the connection was reset. 0 and -1 are different answers and a reader that conflates them either spins forever or stops early. */

/* M65: capabilities.
 *
 * The two calls a program makes about its own authority. There is no
 * third one, and the absence is the design: nothing anywhere grants a
 * capability to a running process. A process is given a set when it is
 * spawned - the kernel applies system_api/include/caps.h's manifest, so
 * no launcher can opt out of it - and from then on the set can only get
 * smaller.
 */
#define SYS_getcaps  57 /* () -> this process's capability bitmask (caps.h's CAP_*). Never fails; a program is always entitled to know what it may do, and one that has to discover its own limits by being refused is a program that reports confusing errors. */
#define SYS_dropcaps 58 /* (mask_to_keep) -> the resulting mask. Intersects; it cannot add. A program that has finished the privileged part of its work drops the rest, which is the one thing a program can do about its own blast radius. Irreversible for the life of the process, and inherited in its reduced form by anything it spawns afterwards. */

/* M56: how many bytes a pipe holds. Part of the ABI because a caller
 * genuinely needs it: SYS_write to a full pipe *blocks*, and a client
 * that cannot afford to block forever - one whose peer may not exist yet,
 * which is every client of a rendezvous pipe - has no other way to ask
 * "is there room". SYS_pipe_poll answers how much is already queued.
 *
 * kernel/ipc/pipe.h defines PIPE_BUF_SIZE as this, so the two cannot
 * drift; this is the copy user space is allowed to see. */
#define SYS_PIPE_CAPACITY 1024

#define SYSCALL_COUNT 65
