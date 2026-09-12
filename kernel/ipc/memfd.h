/* kernel/ipc/memfd.h
 *
 * M120: anonymous shared memory that a descriptor names - the third piece
 * of a multi-process engine's IPC layer, after M118's channel and M119's
 * wait.
 *
 * ---- why this, and why now --------------------------------------------
 *
 * docs/browser.md's measurement lists six absent syscalls that matter, and
 * this is one of them: "`memfd_create` - shared memory between renderer
 * and GPU process". It sits exactly between the two milestones before it.
 * Mojo's transport is a Unix-domain socket (M118) driven by an event loop
 * (M119), and what it *carries* for anything larger than a message is a
 * **handle to memory**: `base::WritableSharedMemoryRegion` is a
 * `memfd_create` plus an `ftruncate`, and what crosses the channel is that
 * descriptor. A bitmap, a video frame, a compositor surface and a V8
 * snapshot all travel this way. With M118 and M119 in place and this
 * missing, a renderer could talk and could wait and could not share a
 * pixel.
 *
 * ---- what this machine already had, and why it was not enough ---------
 *
 * `kernel/ipc/shm.h` has shared memory and has had it since M19 - it is
 * how the compositor and every window client share a pixel buffer. Two
 * differences, and the second is the one that matters:
 *
 *   - **A segment is named by a global id**, an integer in a namespace
 *     every process can see and guess at. That is exactly right for a
 *     compositor protocol where the two sides rendezvous by name, and
 *     exactly wrong for a sandboxed renderer: a process that may not open
 *     a socket can still map segment 7.
 *   - **It is not a descriptor**, so it cannot be passed over a channel,
 *     inherited, counted by the fd table, or closed by `SYS_close`. M118
 *     built descriptor passing; a thing that is not a descriptor cannot
 *     use it.
 *
 * So this is shm's authority model turned the right way round: **memory
 * nobody can name and anybody holding the descriptor can map.** That is
 * the capability shape this OS already argues for everywhere else
 * (docs/capabilities.md), arrived at from the other direction.
 *
 * ---- the lifetime rule, which is the whole of the difficulty ----------
 *
 * `mmap` a memfd, then close the descriptor: the mapping stays valid. That
 * is POSIX, it is what every program does (Chromium's `SharedMemoryMapping`
 * outlives the `Region` that made it), and it is the reason this object is
 * refcounted by *two* different things:
 *
 *   - every descriptor naming it, and
 *   - every mmap region naming it.
 *
 * The second is why `mmap_region_t` grew a tag. A region names a memfd by
 * (slot, generation) rather than by pointer - the pointer would have cost
 * eight bytes in every one of 16,384 region slots, and the tag fits in
 * padding that was already there. The generation is M54's pid trick
 * applied to the same hazard for the same reason: a slot that is freed and
 * handed out again must not make a stale region name *a different
 * object's memory*, which is the one failure here that would be silent
 * data corruption rather than a fault.
 *
 * ---- frames, and the one thing they must not do ------------------------
 *
 * A frame handed to a second process must be zeroed first. Anonymous
 * memory this machine has ever used for anything else would otherwise
 * reach a program that may not read a file, which is the single worst
 * thing this object could do. Zeroed at allocation, in one place, and
 * asserted by a host test rather than by reading the code.
 */
#pragma once

#include <stdint.h>

/* Live objects. A fixed table rather than kmalloc'd objects like M118's
 * sockets, and for a reason rather than by habit: a region names one by
 * slot index, and an index only means something in a table. */
#define MEMFD_MAX 32

/* The largest single region, in pages. 16 MiB.
 *
 * Sized against the machine rather than against Chromium's appetite: the
 * small configuration this project's harnesses must pass in is 128 MiB
 * (QEMU_MEM=128), so one region at this cap is already an eighth of it. A
 * caller asking for more is refused with a number behind it. What would
 * raise it is a measurement of something real being refused - a video
 * frame at this machine's resolution is 3 MB, and a 1024x768 surface is
 * 3 MB, so the cap is five of those. */
#define MEMFD_MAX_PAGES 4096

/* A name, for diagnostics only - `memfd_create`'s first argument, which
 * Linux uses in /proc and nothing here reads back. Kept because a program
 * passes one and because a kernel log line naming the region that filled
 * memory is worth twenty-four bytes. */
#define MEMFD_NAME_MAX 24

/* Seals. Linux's values, and the three that can mean something here.
 *
 * A seal is a promise that cannot be taken back, which is what makes
 * read-only shared memory possible at all: the sender seals writing,
 * passes the descriptor, and the receiver can verify rather than trust.
 * Chromium's `PlatformSharedMemoryRegion::ConvertToReadOnly` is exactly
 * that sequence, and without seals it would be a comment.
 *
 * F_SEAL_SEAL is the fourth and it is honoured too: it forbids adding any
 * further seal, which is how a sender stops the receiver from sealing
 * *more* than was agreed. */
#define MEMFD_SEAL_SEAL   0x0001
#define MEMFD_SEAL_SHRINK 0x0002
#define MEMFD_SEAL_GROW   0x0004
#define MEMFD_SEAL_WRITE  0x0008

struct memfd;

void memfd_init(void);

/* A new, empty object. `name` may be NULL. One reference, which the
 * caller's fd slot takes. NULL if the table is full or the heap is. */
struct memfd *memfd_create_obj(const char *name);

void memfd_ref(struct memfd *m);
void memfd_unref(struct memfd *m);

/* The tag a mapping records: which slot, and which generation of it.
 * Both are needed and the second is the load-bearing one - see the header.
 */
uint8_t memfd_slot(const struct memfd *m);
uint16_t memfd_generation(const struct memfd *m);

/* The object a region's tag names, or NULL if that generation has gone.
 * Takes no reference: the caller is a page fault that is about to map a
 * frame and is holding the object alive through the region's own
 * reference. */
struct memfd *memfd_by_tag(uint8_t slot, uint16_t generation);

/* Sets the size in bytes, allocating zeroed frames or freeing the tail.
 * Returns 0, or -1 for a size past MEMFD_MAX_PAGES, for no memory, or for
 * a seal that forbids it. Growing a mapped region is allowed; SHRINKING
 * ONE IS NOT, and that is not a seal - it is that a mapping whose frames
 * were freed under it would read somebody else's memory at the next
 * fault. Linux answers that case with SIGBUS machinery this kernel does
 * not have, so it is refused instead, which is the conservative half of
 * the same answer. */
int memfd_truncate(struct memfd *m, uint64_t size);

uint64_t memfd_size(const struct memfd *m);

/* The frame holding page `index`, or 0 if that page does not exist. No
 * reference: the frames live and die with the object. */
uint64_t memfd_frame(const struct memfd *m, uint32_t index);

/* Adds seals, or returns -1 if F_SEAL_SEAL is already set or `seals`
 * contains a bit this kernel does not implement. Seals never come off. */
int memfd_add_seals(struct memfd *m, uint32_t seals);
uint32_t memfd_get_seals(const struct memfd *m);

/* May a mapping of this object be writable? F_SEAL_WRITE is the one seal
 * that has to be checked somewhere other than in this file - at mmap, by
 * the caller that knows what `prot` was asked for. */
int memfd_may_write(const struct memfd *m);

/* One more / one fewer mmap region names this object. Separate from
 * memfd_ref only in name: they are the same count, and the two spellings
 * exist so that the call sites say which of the two lifetimes they are
 * about. A mapping outliving its descriptor is the case this exists for. */
void memfd_region_ref(struct memfd *m);
void memfd_region_unref(struct memfd *m);

/* The name this object was created with, never NULL and possibly empty.
 * Diagnostics only - nothing in this kernel decides anything by it - and
 * it exists because the boot self-test's leak check can then say WHICH
 * region was left behind, which is the difference between a number and a
 * lead. */
const char *memfd_name(const struct memfd *m);

/* The name of any object still alive, or "" - what the boot self-test
 * prints when its leak check fails, so the message names the thing rather
 * than counting it. */
const char *memfd_first_live_name(void);

/* For the leak audit: objects alive, and pages they hold between them. */
int memfd_in_use(void);
uint32_t memfd_pages_held(void);
