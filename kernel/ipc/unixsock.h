/* kernel/ipc/unixsock.h
 *
 * M118: AF_UNIX, and the thing it is actually for - passing a descriptor
 * from one process to another.
 *
 * ---- why this file exists, in one paragraph --------------------------
 *
 * docs/browser.md measured what a browser of Chromium's kind asks of
 * this machine and reduced it to five conditions, of which the first is
 * not a number: "`AF_UNIX` with `SCM_RIGHTS`. Without descriptor passing
 * there is no Mojo, and without Mojo there is no Chromium - not a slow
 * one, not a limited one, none." It then said it should be the next
 * thing. Reading the same question again for WebKit, Gecko and Ladybird
 * said something the measurement had not: *all three* pass descriptors
 * over a Unix-domain socket too (WebKit's IPC::Connection, Gecko's
 * IPDL, Ladybird's LibIPC), and none of them has a supported
 * single-process mode any more. So this is not Chromium's condition. It
 * is the price of admission to every multi-process browser engine that
 * exists, and that is what makes it worth building before any of them is
 * chosen.
 *
 * ---- why it is in kernel/ipc and not kernel/net ----------------------
 *
 * Because it is not networking. A Unix-domain socket shares nothing with
 * kernel/net except a spelling: no address, no checksum, no
 * retransmission, no device. What it has in common with kernel/ipc/pipe.c
 * is everything - a bounded buffer between two processes, a refcount that
 * says when the other end is gone, and a wake when bytes land. Putting it
 * beside TCP would have meant one `net_lock` over a channel no packet can
 * reach, and the capability question below would have answered itself
 * wrongly.
 *
 * ---- the capability decision, stated rather than inherited -----------
 *
 * SYS_socket checks CAP_NETWORK, and an AF_UNIX socket does NOT, which is
 * a decision this file is the right place to argue. A capability here
 * answers "what may this program reach" (docs/capabilities.md), and what
 * an AF_UNIX socket reaches is another process on this machine that is
 * already listening for it - which is what a pipe reaches, and pipes need
 * no capability. Gating it on CAP_NETWORK would also have been
 * *precisely* backwards for the thing it is built for: a renderer process
 * is the one program on the machine that must hold no network capability
 * at all, and it is the one program that cannot work without this call.
 *
 * What the capability model does still decide is the descriptors that
 * travel: a passed descriptor carries exactly the authority it already
 * had, because what crosses is a reference to the same kernel object,
 * retained. A process cannot manufacture authority by sending it, and -
 * the half worth saying out loud - a process CAN hand over an authority
 * it holds, which is what the call is for. A capability set still only
 * ever shrinks; this does not add to one, it moves a handle.
 *
 * ---- the shape, and where the parking is -----------------------------
 *
 * Every call in this file is non-blocking, including the two that a
 * program experiences as blocking. That is kernel/net's arrangement
 * rather than pipe.c's, and deliberately: tcp.c never parks a task -
 * syscall.c's read/write loops do, around a non-blocking tcp_recv - which
 * is what makes tcp.c a unit a host test can drive (tests/test_tcp_*.c)
 * instead of a subsystem that needs a scheduler to say anything. This
 * file is graded by tests/test_unixsock.c for the same reason, and the
 * whole of SCM_RIGHTS' bookkeeping is reachable from there.
 */
#pragma once

#include <stdint.h>

#include "os_net.h"      /* system_api/include/os_net.h - OS_MSG_MAX_FDS, which this file's UNIX_MAX_FDS *is* */
#include "sched/sched.h" /* fd_slot_t: what a passed descriptor IS here - see unixsock_send */

/* The two types that are built, and the one that is not.
 *
 * SOCK_STREAM is Mojo's (mojo/public/cpp/platform/socket_utils_posix.cc)
 * and WebKit's; SOCK_SEQPACKET is Chromium's sandbox IPC
 * (base/posix/unix_domain_socket.cc) and Ladybird's. Both are here
 * because both are asked for by the engines this exists for, and because
 * the difference between them is four lines once the message queue below
 * exists at all.
 *
 * AF_UNIX SOCK_DGRAM is NOT built. It is an unconnected socket with an
 * address per datagram, which means a second rendezvous path through the
 * name table for every send rather than once at connect - and nothing
 * this project is aiming at asks for one (syslog does, on a machine with
 * a /dev/log, which this is not). The condition for building it: a
 * program that sends to a bound name without connecting to it first. */
#define UNIX_SOCK_STREAM    1
#define UNIX_SOCK_SEQPACKET 5 /* Linux's number, so that one constant means one thing across the ABI */

/* sockaddr_un's sun_path, and one definition for both sides of the
 * syscall - the near-duplicate cap this project has shipped bugs behind
 * three times (M40, M41, M50). */
#define UNIX_PATH_MAX 108

/* Per-socket receive ring. 4 KiB, and the number is a measurement of
 * nothing - it is a page, it is what a pipe gets four of, and a stream
 * socket that fills it blocks exactly as a pipe does. A message larger
 * than this is refused on SOCK_SEQPACKET (there is nowhere to put it)
 * and chunked on SOCK_STREAM (which is what a stream means). */
#define UNIX_BUF_SIZE 4096

/* Message records in flight. A SOCK_STREAM send with no descriptors
 * attached coalesces into the tail record, so an ordinary byte stream
 * uses one of these however many writes it took; what consumes them is
 * distinct messages on a SEQPACKET socket and sends that carry
 * descriptors. Sixteen is a queue, not a ceiling a caller can feel: a
 * send that finds them all in use reports "would block", the same as a
 * full buffer, and the reader frees them in order. */
#define UNIX_MAX_SEGS 16

/* Descriptors per message - os_net.h's number, not a second one. Eight
 * fd_slot_t per record times sixteen records is the bulk of this object's
 * 4.8 KiB, which is the argument for the value and is made where the
 * value is. A caller that needs more gets EINVAL rather than a silent
 * truncation - see unixsock_send. */
#define UNIX_MAX_FDS OS_MSG_MAX_FDS

/* Connections waiting on a listener, and live sockets overall. The
 * second is a cap rather than a table: these are kmalloc'd like pipes,
 * and the count exists so that "run out of sockets" is a refusal with a
 * number behind it instead of a heap that grows until something else
 * fails (Q9's own lesson - a resource has to refuse, recover, and work
 * again). */
#define UNIX_BACKLOG     8
#define UNIX_MAX_SOCKETS 64

/* Bound names. Abstract names (a leading NUL - Linux's namespace with no
 * filesystem in it) and path names share this table; see unixsock_bind
 * for what a path name here is and is not. */
#define UNIX_MAX_NAMES 16

struct unixsock;

/* Flags unixsock_recv reports back, which are MSG_TRUNC and MSG_CTRUNC's
 * meanings and not their values - the numbers are <sys/socket.h>'s and
 * libc maps them, the same seam that already converts byte order for
 * sockaddr_in (user_space/libc/src/socket.c). */
#define UNIX_RECV_TRUNC  1 /* a SEQPACKET message was longer than the buffer; the rest is discarded, as POSIX requires */
#define UNIX_RECV_CTRUNC 2 /* descriptors came with it that did not fit, and have been closed */

void unixsock_init(void);

/* One unconnected socket, or NULL if the cap or the heap says no. Starts
 * with one reference, which the caller's fd slot takes ownership of. */
struct unixsock *unixsock_alloc(int type);

/* socketpair(2): two sockets already peered, neither of them named. This
 * is the call every engine this file exists for actually uses - a named
 * socket is how an unrelated process finds you, and a pair is what you
 * hand a child you just forked. Returns 0 with both out-parameters set
 * (one reference each), or -1 having created nothing. */
int unixsock_pair(int type, struct unixsock **a_out, struct unixsock **b_out);

void unixsock_ref(struct unixsock *s);
void unixsock_unref(struct unixsock *s);

int unixsock_type(const struct unixsock *s);

/* Claims `name` (a byte string of `len`, not a C string - an abstract
 * name begins with NUL) for this socket. -1 if the socket is already
 * bound, the name is taken, or the table is full.
 *
 * **What a path name here is not.** It is an entry in this table, not a
 * node in leanfs: nothing appears in the directory, stat() on it fails,
 * and unlink() of it does not unbind. That is kernel/ipc/pipe.c's named
 * pipes exactly (pipe_named), it is enough for two programs that agree on
 * a string, and it is recorded as a divergence rather than discovered as
 * one - see docs/unix-sockets.md. The condition for making it a real
 * filesystem node: a program that needs stat() or unlink() on the path to
 * succeed, or two programs rendezvousing through a path neither of them
 * hard-coded. */
int unixsock_bind(struct unixsock *s, const char *name, int len);

/* A bound socket becomes one that accepts. -1 if it is not bound, is
 * already connected, or is SOCK_DGRAM (which does not exist here). */
int unixsock_listen(struct unixsock *s);

/* Peers `s` with a fresh socket queued on whatever listener holds
 * `name`, which is what connect(2) does on this family: the server's end
 * of a connection is a *different* socket from the one that accepted it.
 * 0 on success; -1 if the name is unbound, not listening, a different
 * type, or its backlog is full (ECONNREFUSED in every one of those
 * cases, which is what Linux reports for all of them but the last). */
int unixsock_connect(struct unixsock *s, const char *name, int len);

/* The oldest queued connection, with the backlog's reference handed to
 * the caller, or NULL when none is waiting. Never blocks: the park is
 * syscall.c's, on the readiness unixsock_pending reports. */
struct unixsock *unixsock_accept(struct unixsock *listener);

/* Sends `len` bytes and `nfds` descriptors to the peer's receive queue.
 *
 * Returns the byte count accepted (which for SOCK_STREAM may be short),
 * 0 for "would block - nothing was taken", or -1 when the peer is gone
 * (EPIPE) or the request cannot ever be satisfied (a SEQPACKET message
 * larger than the buffer, more than UNIX_MAX_FDS descriptors).
 *
 * `fds` are fd_slot_t values copied out of the sender's table; this call
 * RETAINS each one it keeps (fd_retain), so the reference the receiver
 * eventually installs is its own and the sender may close its copy the
 * instant this returns - which is what every program that passes a
 * descriptor does. Descriptors are never partially accepted: a send that
 * reports a short count has taken all of the descriptors and some of the
 * bytes, because they belong to the record the bytes start in, and a
 * caller looping on the remainder must not send them twice. */
long unixsock_send(struct unixsock *s, const uint8_t *data, uint32_t len,
                   const fd_slot_t *fds, int nfds);

/* Receives at most `max` bytes and at most `max_fds` descriptors.
 *
 * Returns the byte count, 0 for "nothing right now", or -1 for end of
 * stream - tcp_recv's convention, because syscall.c's read loop is
 * written against it and a second convention in the same function would
 * be a bug waiting for the first reader.
 *
 * Descriptors come out as fd_slot_t values that are ALREADY retained:
 * the caller installs each into its table and owns it, or calls
 * fd_release on the ones it could not install. Descriptors that do not
 * fit in `max_fds` are closed and UNIX_RECV_CTRUNC is reported, which is
 * Linux's behaviour and the only safe one - a descriptor nobody is told
 * about is a leak until the socket dies.
 *
 * On SOCK_STREAM this does not read across a record that carries
 * descriptors: the bytes that arrived with a descriptor are returned with
 * it and the read stops there, so "the message and its handles arrive
 * together" is true for a byte stream as well. That is what Linux does,
 * and it is the property every IPC layer named in this header's first
 * paragraph relies on. */
long unixsock_recv(struct unixsock *s, uint8_t *out, uint32_t max,
                   fd_slot_t *fds_out, int max_fds, int *nfds_out,
                   int *flags_out);

/* Would a read return something other than "nothing"? 1 for queued
 * bytes, a queued connection on a listener, or an end-of-stream that a
 * waiter has to be told about - the same "end of stream is a readable
 * event" rule that stops SYS_waitfds from hanging on a pipe (M68). */
int unixsock_pending(const struct unixsock *s);

/* M119: the three questions epoll asks that `unixsock_pending` folds
 * together. It has to fold them - SYS_waitfds returns one bit and end of
 * stream has to count as readable there, or a waiter hangs - and epoll has
 * to tell them apart, because a pump that cannot distinguish "there are
 * bytes" from "the peer is gone" spins on the second one forever.
 *
 * `unixsock_writable`: the peer's buffer has room, or there is no peer at
 * all - a send that will fail is still a send that will not block, and
 * reporting otherwise parks a writer on a channel that can never take
 * another byte.
 *
 * `unixsock_hup`: this end will never see another byte. The peer is gone
 * or has shut down its write side AND the queue is empty.
 *
 * `unixsock_rdhup`: the peer has finished writing, whether or not the
 * bytes it already sent have been read - EPOLLRDHUP, which is how a pump
 * learns a request is complete while it still has the request to read. */
int unixsock_writable(const struct unixsock *s);
int unixsock_hup(const struct unixsock *s);
int unixsock_rdhup(const struct unixsock *s);

/* shutdown(2). `how` is SHUT_RD (0), SHUT_WR (1) or SHUT_RDWR (2), and
 * unlike the TCP path this one is real: there is no FIN to send, so a
 * half-close here is a flag and a wake. It is in this milestone because
 * an IPC layer that cannot say "I am done writing" without closing the
 * descriptor cannot tell its peer apart from a crash. */
int unixsock_shutdown(struct unixsock *s, int how);

/* How many sockets exist, for the leak audit a table like this needs -
 * the same question openfile_in_use answers, and for the same reason:
 * "the descriptors came back" and "the objects came back" are two
 * claims. */
int unixsock_in_use(void);

/* How many descriptors are sitting in queues, unread. The number that
 * makes "a message nobody read did not leak the descriptors it carried"
 * an assertion rather than an argument. */
int unixsock_queued_fds(void);
