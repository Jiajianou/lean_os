/* kernel/ipc/eventfd.h
 *
 * M119: a counter you can wait on - the smallest of the three objects
 * this milestone adds and the one the other two are measured against.
 *
 * ---- why ---------------------------------------------------------------
 *
 * docs/browser.md's second condition, after M118 closed the first: "an
 * epoll-shaped readiness interface, plus `eventfd`/`timerfd`. The message
 * pump is not optional and `poll` is not what it calls." An `eventfd` is
 * how a thread in that pump is woken by another thread: the waiter is
 * blocked in `epoll_wait` over a set of descriptors, and the only way to
 * interrupt it is to make one of them readable. Chromium's
 * `base::MessagePumpEpoll` owns one, `WakeUp()` writes to it, and a task
 * posted from another thread arrives exactly that way. libevent does the
 * same; so does glib.
 *
 * This project already had the *mechanism* - a pipe is a thing you can
 * make readable by writing one byte to it, and `SYS_pipe` has existed
 * since M14. What it did not have is the name, and the name matters here
 * for a reason beyond compatibility: a pipe costs two descriptors and a
 * 4 KiB buffer to carry one bit, and a wake-up that is written more often
 * than it is read fills that buffer and then blocks the waker. An
 * eventfd's counter saturates instead, which is the behaviour a wake-up
 * flag actually wants.
 *
 * ---- the shape ---------------------------------------------------------
 *
 * A 64-bit counter in the kernel and one descriptor. A write adds to it; a
 * read takes it and returns what it took, blocking while it is zero. Two
 * modes, and the difference is what a read takes:
 *
 *   - the default: the whole counter, which is "how many times was I
 *     poked since I last looked". That is what a wake-up flag wants.
 *   - EFD_SEMAPHORE: exactly one, which makes the object a counting
 *     semaphore. CPython's `_thread` and glib both use this form.
 *
 * Every call here is a pure function of the counter - no clock, no
 * scheduler - which is what lets tests/test_readyfds.c grade all of it
 * off the machine. The parking is syscall.c's, as it is for every other
 * object in this directory (see unixsock.h for the argument).
 */
#pragma once

#include <stdint.h>

/* Live objects. A cap rather than a table: these are kmalloc'd, and the
 * count exists so that running out is a refusal with a number behind it
 * rather than a heap that grows until something else fails. */
#define EVENTFD_MAX 64

/* The value a write may not push the counter to. Linux's rule exactly:
 * the counter saturates one short of 2^64, so that "all ones" is never a
 * value a reader can see and a read of it is unambiguous. A write that
 * would cross it is refused (EAGAIN) rather than wrapped - a wrapped
 * counter is a lost wake-up, which is the one failure mode this object
 * exists to avoid. */
#define EVENTFD_MAX_COUNT 0xFFFFFFFFFFFFFFFEULL

struct eventfd;

void eventfd_init(void);

/* `semaphore` non-zero selects EFD_SEMAPHORE. One reference, which the
 * caller's fd slot takes ownership of. NULL if the cap or the heap says
 * no. */
struct eventfd *eventfd_create(uint64_t initval, int semaphore);

void eventfd_ref(struct eventfd *e);
void eventfd_unref(struct eventfd *e);

/* Takes the counter (or one, in semaphore mode) into `out`. Returns 0, or
 * -1 when the counter is zero - "nothing yet", which syscall.c turns into
 * a park or into EAGAIN. Never blocks. */
int eventfd_read(struct eventfd *e, uint64_t *out);

/* Adds `v`. Returns 0, -1 if it would pass EVENTFD_MAX_COUNT (the
 * caller's cue to wait for a reader), and -2 for a value no write may
 * ever carry: 0 is a no-op on Linux and 2^64-1 is refused there, and both
 * are distinguished from "too big right now" because one of them is a
 * wait and the other never will be. */
int eventfd_write(struct eventfd *e, uint64_t v);

/* Would a read return something? The readable condition, which is what
 * epoll and SYS_waitfds ask. */
int eventfd_readable(const struct eventfd *e);

/* Would a write? A counter at saturation is the only time this is false,
 * and saying so is what makes EPOLLOUT on an eventfd mean something. */
int eventfd_writable(const struct eventfd *e);

/* For the leak audit, like openfile_in_use and unixsock_in_use. */
int eventfd_in_use(void);
