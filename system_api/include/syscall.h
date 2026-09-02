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
#define SYS_exit       1 /* (code) -> never returns. M79: ends every task in the caller's thread group, not just this one - which is what POSIX exit() means and what `return` from main has always meant. SYS_thread_exit is the one that ends a single thread. */
#define SYS_getpid     2 /* () -> the caller's thread GROUP id. Identical to the task's own id for anything that is not a thread, which is every process this OS ran before M79 - so nothing that used this before means anything different now. SYS_gettid is the per-thread answer. */
/* M60: the most arguments SYS_spawn will carry. A vector longer than
 * this is truncated at the last whole argument rather than refused - a
 * program seeing fewer arguments than it was given is a failure it can
 * report, and half an argument names something else. Sixteen is the
 * width of every command line this OS's terminal can produce and then
 * some; the real ceiling is the single page the vector is copied into. */
#define SPAWN_MAX_ARGS 16

#define SYS_spawn      3 /* (path, argv, envp) -> pid or -1 (a SPAWN_ERR_* code, M48). Combines fork+exec into one call - a fork/exec "equivalent" (M13), not literal fork(): no address-space duplication, just a fresh process loaded straight from a named file. M60: `argv` is a NULL-terminated array of char* holding the arguments *after* the program name - the kernel puts `path` in argv[0] itself, because that is the one element it knows for certain and the one a program is entitled to assume is there. NULL is a program launched with no arguments. Truncated at SPAWN_MAX_ARGS, or at whatever fits in the argument region the vector is copied into; the one-string form every caller in this project used until now is user_space/lib's sys_spawn(), which builds a two-element vector. M75: `envp` is a NULL-terminated array of "NAME=value" strings, or NULL to give the child a copy of the *caller's own* environment - which is what makes an environment a thing that survives a spawn rather than a thing each program invents. user_space/lib's sys_spawnv() passes `environ`, so a setenv() before a spawn is visible to the child; the NULL form is what a kernel thread and every pre-M75 caller get, and it means "whatever I was started with". M75: the child also inherits the caller's working directory (SYS_chdir), which is the other half of "a place to stand" - the pair is what lets a launcher say where a program should run without inventing an argument for it. */
#define SYS_wait       4 /* (pid) -> exit code. M68: a real blocking wait - a parent waiting on one child parks on that child and is woken by it. (This said "polls + cooperatively yields rather than a real blocking wait queue - M14 is where more complete wait semantics is scoped to land" for fifty-four milestones after M68 made it false. M84 is what had reason to read it.) Returns an exit code, which cannot distinguish exit(139) from a death by SIGSEGV - SYS_waitpid is the call that can. */
#define SYS_read       5 /* (fd, buf, len) -> bytes read. Only fd=0 (stdin/keyboard) is wired up; blocks (yields) until at least one byte is available. */
#define SYS_readfile   6 /* (name, buf, maxlen) -> bytes copied or -1. Whole-file read by name - no open/close/fd-table/lseek yet, matching M13's "a couple of coreutils" scope rather than a full VFS API nothing needs yet. */
#define SYS_listdir    7 /* (path, buf, maxlen) -> bytes written or -1. Newline-separated names of everything in the directory at `path`, with a '/' appended to each one that is itself a directory so a caller can tell the two apart without a second call. M53: this *was* SYS_listfiles(buf, maxlen), which took no path because leanfs was flat and "the entire namespace" was the only answer it could give. Same number, new signature - one repo, every caller converted in the same commit, and leaving a second call that only ever means "/" would just be a way for the two to drift. */
#define SYS_kill       8 /* (pid, sig) -> 0 or -1. M76: every signal in signal.h, not just the two that kill. What happens on arrival depends on the target: a handler runs it (SYS_sigaction), SIG_IGN_ADDR drops it, and the default action is to terminate the process with 128+sig - except SIGCHLD, whose default is to be ignored. sig 0 delivers nothing and just answers "does this pid exist and may I signal it", which is what every `kill -0` in the world is for. Delivery is still not asynchronous in the interrupt sense: a fatal signal is taken at the next syscall entry or scheduler tick, and a *caught* one at the next return from a syscall to ring 3 - so a program in a pure compute loop that never syscalls does not run its handler until it does. Said plainly here because it is the one place this differs visibly from a real Unix, and the fix (a per-CPU check on the ring-3 return path of every interrupt) is a real piece of work rather than an oversight. */
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
/* M84: close this descriptor when the process execs.
 *
 * A bit of its own in the OPEN_* space, and that is the whole point.
 * FD_CLOEXEC_BIT (below) is the value F_SETFD takes and is 1, because
 * that is what POSIX says and what every program passes; OPEN_READ is
 * also 1, because that is what this ABI has said since M59. They are two
 * different namespaces and defining O_CLOEXEC as FD_CLOEXEC_BIT - which
 * is what the first draft of M84 did - makes `open(path, O_RDONLY)` mark
 * a descriptor close-on-exec and `open(path, O_WRONLY|O_CLOEXEC)` mean
 * O_RDWR. Caught by reading the numbers rather than by running it, which
 * is the only way that one gets caught. */
#define OPEN_CLOEXEC  0x20
/* M87: fail if the file already exists. Only meaningful with
 * OPEN_CREATE, which is the pairing every open(2) documents - and the
 * reason it exists at all is that "check then create" is a race and
 * this is not: the check and the create happen inside one critical
 * section of the filesystem lock, so of two processes that both ask,
 * exactly one gets the file. <fcntl.h> defined O_EXCL as 0 for
 * twenty-eight milestones with a note that a program relying on it
 * "gets no protection". It does now. */
#define OPEN_EXCL     0x40

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

#define SYS_klog 65 /* (from_position, char *buf, max, uint64_t *next_out) -> bytes copied, or -1. M70: read the kernel log back. There has never been a way to - klog writes to a serial port and to a console the compositor paints over, so on a machine with no serial cable this kernel's entire diagnostic surface was a black screen, including every capability denial M65 was careful to emit. `from` is an absolute byte position and `*next_out` (may be NULL) comes back as where to resume, so a reader follows the log rather than re-reading it; a reader that has fallen further behind than the ring holds is advanced to the oldest surviving byte and can tell, because the cursor jumps. Gated on CAP_SYSLOG: the log describes what every other process on the machine is doing, which is authority rather than a fact about the hardware - see caps.h on why SYS_fb_info and SYS_netconf are deliberately not gated and this is. */
#define SYS_klog_total 66 /* () -> total bytes ever logged. A reader that wants only what happens from now on starts here instead of at 0, without having to drain everything first. Not gated: it is a length, not a content. */

#define SYS_rename_replace 67 /* (old_path, new_path) -> 0 or -1. M71: the half SYS_rename refuses. M56 was right that silently replacing a file is a way to lose one, and that is why the default still refuses - but it also makes write-to-a-temp-then-rename impossible, and that dance is the only way to replace a file's contents without a window in which neither version exists. SYS_writefile has that window by construction: it truncates and then writes, so a power cut in the middle loses the new document AND the one it was replacing. This repoints the directory entry in place - one record changes from the old inode to the new one - so the name never stops resolving. Refuses to replace a directory, and replacing a file with itself is a no-op rather than an unlink. */

/* ---- M68 -------------------------------------------------------------- */

#define SYS_waitfds 68 /* (const int *fds, count, timeout_ms) -> the index into `fds` of one that is ready, -2 if the timeout expired with none ready, -1 for a bad argument. THE call this milestone exists for. Every GUI process on this machine - the compositor and every wmclient program - is a loop of non-blocking polls over a handful of pipes followed by SYS_yield, which means it is permanently runnable and no core ever halts. This is the blocking form: it sleeps until one of the descriptors would not block, or until the deadline, and a task inside it is TASK_BLOCKED rather than READY. A pipe read end is ready when it has bytes or its last writer went away (end of stream is a readable event - a reader that could not be woken by it would hang at exactly the moment it should stop); a socket is ready when a datagram or segment is queued; fd 0 is ready when a keystroke is buffered. count is capped at MAX_FDS, and M88 allows it to be 0 - a wait with nothing that could satisfy it early is a sleep, which is what `poll` with an empty set and a timeout means and what user space had no other way to ask for. timeout_ms of 0 polls and returns immediately, which makes this a strict superset of the poll loops it replaces; a negative timeout waits with no deadline. Deliberately returns ONE index rather than a bitmask: the callers here service one channel per pass and loop, and a mask would be an API that promises a fairness this scheduler does not implement. */
#define SYS_idle_ticks 69 /* (int cpu) -> how many timer ticks that CPU has spent with nothing runnable, or -1 for a cpu id that does not exist. The measurement M68 is about, and it exists as a syscall because the claim "this machine sleeps when idle" is not observable any other way - a desktop that spins and one that halts look identical from the outside, which is precisely how this OS shipped sixty-seven milestones without anyone noticing. Pair it with SYS_uptime_ms for a percentage. */

#define SYS_chdir 70 /* (path) -> 0 or -1. M75: the working directory this kernel's own kernel/fs/leanfs.h said it did not have ("every call below takes an absolute path"). Changes the *calling process's* directory, inherited by anything it spawns afterwards, and refuses anything that is not an existing directory - so a cd that succeeded and a cd that did nothing cannot be confused. The path may itself be relative, and "." and ".." are resolved here rather than in leanfs, which deliberately does not store either (see resolve() in kernel/fs/leanfs.c): a working directory is a property of a *caller*, and the filesystem has none. */
#define SYS_getcwd 71 /* (buf, maxlen) -> the length written (not counting the NUL), or -1 if the caller's directory does not fit in maxlen. Always an absolute, normalized path - "/" at boot, and never with a trailing slash unless it IS the root. Refusing a short buffer rather than truncating, for the same reason SYS_spawn refuses a long path: a truncated path names a different directory. */

#define SYS_sigaction 72 /* (signo, handler, restorer) -> the previous handler, or -1. M76: install a ring-3 handler for `signo`. `handler` is SIG_DFL_ADDR (terminate, except SIGCHLD which is ignored), SIG_IGN_ADDR, or a user-space function address; `restorer` is where the kernel should make the handler `ret` to - user_space/lib/crt0.asm's __lean_sigreturn, passed in rather than assumed at a fixed address, because a kernel that hardcoded a user-space address would be a kernel with an opinion about how programs are linked. SIGKILL and SIGSEGV are refused (signal.h's SIG_IS_CATCHABLE says why). Handlers are NOT inherited across SYS_spawn: a fresh image has never seen the address the parent installed, and pointing at it would be pointing into somebody else's program. */
#define SYS_sigreturn 73 /* (sig_frame_t *frame) -> does not return normally. M76: restores the interrupted context the kernel saved on the caller's own stack before it entered a handler, including the pre-handler blocked mask. Called only by the restorer SYS_sigaction was given; a program has no reason to call it and a malformed frame is refused rather than obeyed - every field it restores lands in a ring-3 iretq frame, so this is the one syscall whose argument is, by construction, a way to set your own registers. That is not a privilege escalation here (a process can already set its own registers by executing instructions), but RFLAGS is masked to the bits a ring-3 program may legitimately change. */
#define SYS_sigprocmask 74 /* (how, mask, uint32_t *old_out) -> 0 or -1. M76: SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK against a plain 32-bit word, not POSIX's sigset_t. SIGKILL and SIGSEGV cannot be blocked and are silently cleared from any mask, rather than the call being refused - refusing would make a program that blocks "everything" fail for a reason it cannot act on. */

#define SYS_fstat 75 /* (fd, os_stat_t *out) -> 0 or -1. M77: the half of SYS_stat a `struct stat` needs and a path cannot give - what is this OPEN FILE, asked of a descriptor whose name may since have been renamed out from under it. Only an FD_FILE descriptor has an answer; a pipe, a socket and stdin/stdout are refused rather than described with invented numbers, because a size and an mtime for a pipe would be two lies where -1 is one honest answer. */

#define SYS_mmap   76 /* (addr, len, prot, flags, fd, offset) -> a page-aligned address, or (uint64_t)-1. M78: the second memory primitive next to M19's SYS_sbrk, which is growth-only and says so - "a free-list allocator built on top never needs to give pages back" was true right up until something wanted the pages back. M91 gave it the three arguments every other Unix has had since 4.2BSD. `addr` 0 means "anywhere" and is what every caller before M91 passed; a non-zero `addr` without MAP_FIXED is a hint, honoured if the range is page-aligned, inside the arena and free, and ignored otherwise, which is what a hint means; with MAP_FIXED it is a requirement, and - per every Unix since SunOS - it REPLACES whatever was mapped there rather than refusing, which is how a loader lays one segment of an object over the tail of another's page reservation. `fd` must be negative and `offset` zero: a file-backed mapping is refused by name rather than quietly given anonymous zeroes, for the same reason MAP_SHARED is (kernel/ipc/shm.h is what two processes share memory through). `prot` is honoured completely as of M91 - PROT_WRITE maps writable, PROT_EXEC maps executable and its absence maps NX (there is a real execute-disable bit now), and PROT_NONE is a mapping rather than a refusal: the region exists, no page is ever built for it, and touching it is fatal, which is a guard page that actually guards. Pages arrive on a fault (M82), not at the moment of the call. */
#define SYS_munmap 77 /* (addr, len) -> 0 or -1. M78: gives the frames back and leaves a hole the next SYS_mmap can use - the one thing eleven milestones of a bump-allocated heap structurally could not do. The range may cover part of a mapping, all of one, or several: a prefix or suffix shrinks, an interior range splits one mapping into two, and a range that overlaps nothing is a success rather than an error (there is nothing left to remove, which is what the caller asked for). Only addresses inside the caller's own mmap arena are accepted, so this can unmap what it made and nothing else - not its code, not its stack, not somebody else's shm. */

/* ---- M79: two threads, one address space --------------------------------
 *
 * The first time two of this scheduler's tasks share a page table. Worth
 * stating on its own rather than as an implementation detail, because "a
 * task IS a process" is exactly the assumption a from-scratch scheduler
 * bakes in without anyone deciding to - and every one of the three calls
 * below exists because some part of this kernel had made it.
 */
#define SYS_thread_create 78 /* (entry, arg, stack_top) -> the new thread's tid, or -1. A second schedulable context in the CALLER's address space: same page table, same memory, its own kernel stack and its own ring-3 stack (which the caller supplies, from M78's mmap arena - the kernel does not allocate one, because the per-process layout has room for exactly one stack and a thread's has to come from somewhere a program can also free). `entry` is called with `arg` in RDI, exactly as a spawned program's main is. It must not return: the caller's runtime wraps it (user_space/libc/src/pthread.c) so that returning means SYS_thread_exit. */
#define SYS_thread_exit 79 /* (value) -> never returns. Ends THIS thread and nothing else, unlike SYS_exit which ends the whole thread group the way POSIX exit() does. The address space survives until the last task using it goes - see kernel/sched/sched.c's task_exit_with_code, where "am I the last user of this page table" replaced "am I a process". */
#define SYS_gettid 80 /* () -> this task's own id. SYS_getpid answers with the thread GROUP's id, which is what a program means by "my pid" and what it was already returning before threads existed (a process is its own group). The two differ only inside a thread, which is exactly when the difference matters - raise() has to reach the calling thread, not the leader. */

/* M81: (path, uint32_t *cookie, buf, buflen) -> bytes written into buf, 0 at the end of the directory, or -1.
 *
 * The streaming counterpart to SYS_listdir (7), which is whole-shot: it
 * writes every name in a directory, newline-separated, with a '/' on the
 * ones that are directories, into one buffer sized by the caller. That
 * shape was built for a person reading a terminal and <dirent.h> said so
 * in as many words; it also could not survive M81 raising the file count
 * from 192 to 8192, because "size the buffer for the whole directory" is
 * not something a caller can do when a directory can hold thousands of
 * names of up to 255 bytes.
 *
 * So this one fills whatever buffer it is given with as many whole
 * os_dirent_t records as fit and reports where it got to in `cookie` -
 * zero to start, and handed straight back on the next call. A partial
 * record is never written: a caller that gets fewer bytes than it asked
 * for got fewer entries, not half of one.
 *
 * SYS_listdir stays, and is not deprecated. `ls` and the terminal want a
 * blob of newline-separated names and building one out of records would
 * be longer than asking for it - and since M81 both are the same walk
 * inside the kernel, so there is no second implementation to drift. */
#define SYS_getdents   81

/* M83: () -> the child's pid in the parent, 0 in the child, or -1.
 *
 * The real thing, and the sentence M13 and M80 both wrote has to be
 * retired: this OS *did* have no fork, and SYS_spawn (3) is still a
 * combined fork+exec and still the right call for "start this program".
 * What changed is M82. A fork without demand paging means copying an
 * entire address space eagerly at every call, and M13 was right to refuse
 * that; with a fault handler that can populate, the copy becomes
 * copy-on-write and the trade is a different one.
 *
 * The child gets: a copy-on-write clone of the parent's image, stack,
 * heap and mmap arena - and nothing else that happens to be mapped, so
 * shared memory and the framebuffer window are NOT inherited (the frames
 * behind those belong to shm.c and the compositor, not to the process).
 * It gets the parent's descriptors, working directory, environment,
 * capabilities, signal handlers and signal mask, and its floating-point
 * state. It does not get the parent's pending signals, and it is always a
 * single-threaded process even when the caller was one thread of several
 * - both of which are what POSIX says and both for reasons written down
 * at task_fork.
 *
 * Takes no arguments and cannot, because the value it returns is the only
 * thing that differs between the two sides. */
#define SYS_fork       82

/* M84: (path, argv, envp) -> does not return on success, -1 on failure.
 *
 * Replaces the calling process's memory with a different program's and
 * changes nothing else: same pid, same parent, same process group, same
 * working directory, same descriptors except those marked FD_CLOEXEC.
 * Signal handlers go back to the default (their addresses were in memory
 * that no longer exists) while an explicit "ignore" survives; the
 * capability set is intersected with the new program's manifest, never
 * widened.
 *
 * A `#!` script is refused rather than resolved. SYS_spawn resolves one
 * because it is the call every launcher on this desktop goes through;
 * exec is the call a program makes about itself, and a kernel with an
 * opinion about interpreters is a kernel doing the shell's job.
 *
 * Refused from a process with more than one thread: the other threads
 * are running on the address space this would destroy, and stopping a
 * thread on another core needs machinery this kernel does not have (the
 * same limit SYS_fork states).
 *
 * `envp` NULL means inherit, exactly as SYS_spawn's does. */
#define SYS_execve     83

/* M84: (pid, int *status, options) -> the pid reaped, 0, or -1.
 *
 * `pid` is -1 for any child, or a specific child's pid. A negative pid
 * other than -1 is refused rather than quietly treated as -1: that form
 * means a process group, and process groups arrive with M85.
 *
 * `status` may be NULL. Otherwise it receives the familiar encoding, and
 * it is familiar on purpose - every W-macro ever written assumes it: a
 * normal exit is the code in bits 8-15 with the low seven bits clear; a
 * death by signal is the signal in the low seven bits. <sys/wait.h>
 * decodes it.
 *
 * `options` is WNOHANG (1) or 0. With WNOHANG and a child that is alive,
 * the answer is 0 rather than a block - which is the difference between
 * a shell that can report a background job and one that stops to wait
 * for it.
 *
 * This is what SYS_wait (4) could not be. That call returns an exit code
 * and an exit code cannot tell exit(139) from a death by SIGSEGV, since
 * 128+signal is exactly the convention that makes a signal death readable
 * and therefore ambiguous. SYS_wait stays: every caller in this project
 * uses it and means it. */
#define WNOHANG 1
#define SYS_waitpid    84

/* M84: (fd, cmd, arg) -> the answer, or -1.
 *
 * One descriptor flag exists on this machine and this is the call that
 * reads and writes it. F_GETFD returns FD_CLOEXEC or 0; F_SETFD sets or
 * clears it. Everything else is -1, which is what <fcntl.h> already told
 * a program to expect - and it is now -1 because the flag does not
 * exist rather than because the whole call was a stub.
 *
 * F_GETFL/F_SETFL stay in libc and stay answering 0: O_NONBLOCK is the
 * flag they are about, every descriptor here is blocking, and M88 is
 * where that changes. */
#define F_GETFD_CMD 1
#define F_SETFD_CMD 2
#define FD_CLOEXEC_BIT 1
#define SYS_fcntl      85

/* M85: process groups and sessions - see sys_setpgid in
 * kernel/arch/x86_64/syscall.c for the rules and why each exists.
 *
 * SYS_getpgid (10) has been here since M13 with a note saying it was
 * read-only because there was "no job control to ever need changing it
 * yet". This is that milestone. */
#define SYS_setpgid    86 /* (pid, pgid) -> 0 or -1. pid 0 is the caller; pgid 0 means "lead your own group" */
#define SYS_setsid     87 /* () -> the new session id, or -1 if the caller already leads a group */
#define SYS_getsid     88 /* (pid) -> the session id, or -1. pid 0 is the caller */

/* M85: (fd, cmd, arg) -> the answer, or -1. The first ioctl on this
 * machine, and deliberately the narrow one it looks like rather than a
 * general escape hatch: five commands, each because something concrete
 * needs it. TCGETS/TCSETS are how an editor turns off canonical mode and
 * puts it back, TIOCGWINSZ is how anything that draws finds out how big
 * the screen is, and TIOCGPGRP/TIOCSPGRP are how a shell hands the
 * terminal to a job and takes it back. See system_api/include/termios.h.
 *
 * `fd` must name the terminal - which on this machine means fd 0, 1 or 2,
 * the descriptors every task starts with. That is the same approximation
 * <unistd.h>'s isatty has made since M77, and it stops being an
 * approximation when M87 gives the terminal a path. */
#define SYS_ioctl      89

/* M87: (fd, length) -> 0 or -1. Sets a file's size.
 *
 * <unistd.h> declined to declare this for ten milestones with a note
 * that "a declaration with no implementation would be worse than its
 * absence: a program that probes for it at configure time would find it
 * and then fail to link", and observed that leanfs_handle_truncate had
 * been sitting there the whole time with no syscall exposing it. This is
 * that syscall, and it does more than the function it exposes: growing a
 * file only changes its size, because an unallocated block already reads
 * as zeros. */
#define SYS_ftruncate  90

/* M87: symbolic links.
 *
 * SYS_symlink(target, path) creates `path` pointing at `target` - note
 * the order, which is the one symlink(2) uses everywhere and is the
 * reverse of what most people guess. SYS_readlink(path, buf, len) reads
 * a link's target without following it, and SYS_lstat is stat that stops
 * at a final link instead of resolving it.
 *
 * Those last two are the whole reason a program can tell a link from
 * what it points at. Every other path-taking call here follows, with a
 * hop limit - a chain that is too long, or a link that points at itself,
 * is refused rather than walked further.
 *
 * Deliberately no SYS_link: hard links need a link count in the inode
 * and an unlink that decrements rather than frees, which is a different
 * change touching every path that removes a file. M81 left the room for
 * the count; nothing has used it yet. */
#define SYS_symlink    91
#define SYS_readlink   92
#define SYS_lstat      93

/* M81: the record SYS_getdents writes, and the two limits that go with
 * it. Kept here rather than in a header of its own because it is part of
 * one syscall's contract and nothing else refers to it.
 *
 * `reclen` is the distance to the next record, so a caller walks the
 * buffer without knowing what a name length is; it is rounded up to a
 * multiple of 8 so every record after the first is aligned. The name IS
 * NUL-terminated here, unlike its on-disk form - a kernel that has
 * already copied the bytes can afford the terminator, and every caller
 * in user space wants a C string. */
#define OS_NAME_MAX 255      /* the longest name leanfs stores - matches <dirent.h>'s NAME_MAX */
#define OS_DT_UNKNOWN 0
#define OS_DT_DIR     4      /* the same values <dirent.h> uses, so libc's readdir copies rather than translates */
#define OS_DT_REG     8

typedef struct {
    unsigned int   ino;      /* the inode number, which since M81 is a real one */
    unsigned short reclen;   /* bytes from here to the next record; a multiple of 8 */
    unsigned char  type;     /* OS_DT_* */
    unsigned char  name_len; /* not counting the NUL */
    char           name[];   /* name_len bytes, then a NUL, then padding to reclen */
} os_dirent_t;

/* The most bytes one record can take: the header, the longest name, its
 * NUL, and up to 7 bytes of alignment padding. A caller whose buffer is
 * at least this big can never be told "nothing fits". */
#define OS_DIRENT_MAX (8 + OS_NAME_MAX + 1 + 7)

/* ---- M91 -------------------------------------------------------------- */
#define SYS_mprotect   94 /* (addr, len, prot) -> 0 or -1. M91: change the permissions of an existing mapping. Bounded to the caller's own mmap arena, exactly as SYS_munmap is, and the range must be entirely covered by mappings the caller holds - a partial range is refused rather than half-applied. The interesting half is that this is NOT just a page-table rewrite: in a demand-paged address space most of a mapping has no entry yet, so the region's own prot is updated too, and it is that record the fault handler consults when an untouched page is finally read. Changing only what exists would leave a mapping whose first half is read-only and whose second half turns writable the moment it is touched. PROT_EXEC is honoured for real since M91 (there is an NX bit now), so making a page executable and then not writable is a thing a program can actually do here - which is what a JIT needs and what a loader does to its own relocation table. */
#define SYS_fsync      97 /* (fd) -> 0 or -1. M93, and M87's seventh bullet finally. What it actually guarantees on this machine is narrower than the name suggests and is worth writing down rather than implying: leanfs is write-through (every SYS_write reaches the device before it returns) and M92's block cache is write-through too (it never holds a write the disk has not taken), so a file's DATA is already durable by the time a program could think to call this. What is not, until something flushes it, is the metadata the write dirtied - the inode's size and block pointers, and the allocation bitmap. So this pushes those, which is the only part of "make it durable" that was ever outstanding. Refused for anything that is not an open file: a pipe, a socket and a terminal have nothing on a disk to sync, and returning 0 for them would be agreeing to something. */

#define SYS_link       96 /* (old_path, new_path) -> 0 or -1. M93: a second directory record naming the file `old_path` already names, and one more on its inode's link count. Refused for a directory (a directory with two parents is a cycle, and this filesystem has no `..` to make one visible), for a name that already exists, and for anything under a synthetic mount. Removing one name of several removes only that name - SYS_unlink frees the blocks when the last one goes, which is the whole reason the count exists. M87's fourth bullet asked for this and shipped only the symbolic kind. */

#define SYS_madvise    95 /* (addr, len, advice) -> 0 or -1. M91: MADV_DONTNEED drops every frame in the range and leaves the mapping - the address stays reserved, keeps its permissions, and reads as zero again on the next touch. Every other advice value is accepted as a no-op success rather than refused: this kernel has no page cache for MADV_WILLNEED to warm, and a program that fails because it asked politely is a worse outcome than one whose hint went nowhere. */

/* M101: the profiler's control and readout - see system_api/include/profile.h
 * for the operations, the structs and why this is a syscall rather than a
 * writable /proc file. Gated on CAP_PROCESS_LIST: a flat profile is a
 * description of what every process is executing, which is strictly more
 * than the task list that bit already guards. */
#define SYS_profile    98

/* ---- M88 (second attempt): the three calls a build probes for --------
 *
 * `configure` runs all three within its first hundred tests, and until
 * this milestone this machine could not answer any of them: it had no
 * per-process CPU time, no way to say how big its own disk was, and no
 * way to set a modification time to anything but now. Each is one
 * question the kernel already knew the answer to and had never been
 * asked. */

#define SYS_rusage 99 /* (int who, os_rusage_t *out) -> 0 or -1. OS_RUSAGE_SELF or OS_RUSAGE_CHILDREN (system_api/include/proc.h). Where a process's CPU time went, in PIT ticks, split by the privilege level the timer interrupted - which is what both times() and getrusage() are made of, so this is one call rather than two nearly identical ones. Ticks, not microseconds: a tick is what this machine actually observes (sched_account_tick is called from the timer interrupt), and a finer unit would be arithmetic on a number nothing measured. _SC_CLK_TCK reports the same 100 Hz, which is what lets the receiving side turn one into the other. The CHILDREN form reports what has been REAPED and nothing else - an unreaped child's time is still accruing, and POSIX specifies the same. A THREAD is not a child: a joined thread's ticks are added to the process's own totals rather than to its children's, because a thread's CPU time is time this process spent. That makes SELF mean the calling thread plus every thread of this process already joined - a running sibling's time is not in it, which is stated here rather than left to be discovered. Needs no capability: a process asking where its own CPU time went is asking about itself. */

#define SYS_statvfs 100 /* (const char *path, os_statvfs_t *out) -> 0 or -1. How big the filesystem behind `path` is and how much of it is left, in blocks and in inodes. Both matter here and the second one is the interesting half: LEANFS_MAX_INODES is fixed at format time, so a tree of small files exhausts inodes long before blocks, and a caller watching only free space would watch the wrong ceiling. Refused for a path under a synthetic mount rather than answered with zeros - "how full is /proc" is a question that does not apply, and the zeros a Unix box conventionally returns for it are a number a program will divide by. Refused too for a path that does not exist, so a typo cannot look like a success. */

#define SYS_utime 101 /* (const char *path, uint32_t mtime) -> 0 or -1. Sets a file's modification time to something other than now, which is the one thing every other write path in this filesystem cannot do: they all stamp rtc_now(), correctly, and a build system needs the exception. `make` decides what to rebuild by comparing mtimes, and an unpack or an `install -p` that restamped every file it restored would make the next build rebuild the world. Follows symbolic links (the link's own times are lutimes()' business and nothing has asked); refused under a synthetic mount, where there is no stored time to set. Needs CAP_FS_WRITE: it changes what is on the disk, which is the line that bit draws. */

#define SYSCALL_COUNT 102
