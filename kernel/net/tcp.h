/* kernel/net/tcp.h
 *
 * M66: TCP.
 *
 * The stretch-goal entry that asked for this refused to let it be a
 * bullet point on somebody else's milestone - "retransmission,
 * congestion control and an eleven-state machine are not a bullet on
 * somebody else's list" - and that judgement is why this is its own
 * file rather than a mode of udp.c. What is here:
 *
 *   - The eleven states of RFC 793's figure 6, with the transitions
 *     driven by segment arrival and by a 100 ms timer.
 *   - Sequence numbers, an ISN that is not zero, and acceptance tests
 *     on both the sequence and the acknowledgement of every segment
 *     that arrives.
 *   - A send buffer, retransmission with exponential backoff, and a
 *     retransmission timeout estimated from measured round trips
 *     (Jacobson/Karels, RFC 6298) rather than fixed.
 *   - A receive buffer, a window advertised from the space left in it,
 *     and a sender that respects the window it is given.
 *   - Slow start, congestion avoidance, fast retransmit and fast
 *     recovery - Reno, which is the algorithm the entry means when it
 *     says "congestion control".
 *   - MSS negotiated from the option on SYN, clamped to what this
 *     stack's 1500-byte MTU can carry.
 *   - TIME_WAIT, with the 2*MSL wait it exists for, shortened to
 *     something a self-test can observe and documented as such.
 *
 * What is deliberately not here: window scaling, SACK, timestamps,
 * PAWS, Nagle, delayed ACKs beyond one segment, urgent data, and
 * out-of-order reassembly. The last is the interesting omission - a
 * segment that arrives ahead of a gap is dropped rather than held, which
 * is correct-but-slow behaviour that RFC 793 explicitly permits, and it
 * halves the size of the receive path. Every one of these is a
 * performance feature except SACK, and a stack with no consumers that
 * needs performance is a stack optimising ahead of a measurement.
 */
#pragma once

#include <stdint.h>

/* RFC 793 figure 6. CLOSED is 0 so a zeroed control block is a closed
 * connection, the same reason TASK_FREE is 0 in the scheduler. */
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

/* Per connection. 4 KiB each way is enough to keep a loopback transfer
 * from being one segment at a time and small enough that MAX_TCBS of
 * them is a kernel BSS figure worth having. */
#define TCP_SEND_BUF   4096
#define TCP_RECV_BUF   4096
#define TCP_MAX_TCBS   16
#define TCP_DEFAULT_MSS 536  /* RFC 1122's floor, used until a peer says otherwise */
#define TCP_MAX_MSS    1460  /* 1500 MTU - 20 IP - 20 TCP */

/* How often tcp_tick runs. Everything below counts in these. */
#define TCP_TICK_MS 100

struct tcpcb;

void tcp_init(void);

/* The 100 ms clock: retransmission, TIME_WAIT expiry, connection
 * timeouts. Driven by a kernel thread rather than the PIT hook, which
 * the scheduler owns. */
void tcp_tick(void);

/* Called by ip.c for IP_PROTO_TCP. */
void tcp_handle_packet(uint32_t src_ip, uint32_t dst_ip, const uint8_t *segment, uint16_t len);

/* ---- what the socket layer above calls -------------------------------
 *
 * Every one of these is non-blocking and returns immediately; the socket
 * layer and the syscalls above it poll, exactly as they do for a
 * datagram socket. A blocking connect() would need the wait-queue
 * machinery this kernel has never had, and building one for TCP alone
 * would be a scheduler change hiding inside a networking milestone. */

struct tcpcb *tcp_open(void);
void tcp_close(struct tcpcb *tcb);            /* active close: FIN, then run the state machine down */

/* The socket layer has let go of this control block. tcp.c reclaims the
 * slot when the state machine is also finished with it, which may be
 * before this call (a refused connection) or long after (TIME_WAIT).
 * Never dereference the pointer again after calling this. */
void tcp_release(struct tcpcb *tcb);
void tcp_abort(struct tcpcb *tcb);            /* RST and free - what a dropped fd on a half-open connection does */

int tcp_bind(struct tcpcb *tcb, uint16_t port);
int tcp_listen(struct tcpcb *tcb);
int tcp_connect(struct tcpcb *tcb, uint32_t ip, uint16_t port);

/* A connection that completed its handshake on a listening tcb, or NULL.
 * The caller takes ownership. */
struct tcpcb *tcp_accept(struct tcpcb *listener);

/* How many completed connections are waiting on a listener - what a poll
 * on a listening socket answers, and the difference between "accept
 * would succeed" and "accept would return nothing". */
int tcp_accept_pending(const struct tcpcb *listener);

/* Queues as much of `data` as the send buffer has room for; returns how
 * much, which may be 0 and may be less than `len`. Partial writes are
 * the honest answer for a non-blocking send and the caller loops. */
int tcp_send(struct tcpcb *tcb, const uint8_t *data, uint16_t len);

/* Copies out at most `max` received bytes; returns how many, 0 if none.
 * -1 once the peer has closed *and* the buffer is drained, which is the
 * end-of-stream a reader needs and is a different answer from 0. */
int tcp_recv(struct tcpcb *tcb, uint8_t *out, uint16_t max);

int tcp_bytes_available(const struct tcpcb *tcb);
int tcp_send_space(const struct tcpcb *tcb);
tcp_state_t tcp_state(const struct tcpcb *tcb);
uint16_t tcp_local_port(const struct tcpcb *tcb);
uint32_t tcp_remote_ip(const struct tcpcb *tcb);
uint16_t tcp_remote_port(const struct tcpcb *tcb);

/* 1 once a connect() has succeeded or failed for good, so a caller can
 * poll one thing rather than enumerate states. tcp_connect_failed says
 * which. */
int tcp_connect_settled(const struct tcpcb *tcb);
int tcp_connect_failed(const struct tcpcb *tcb);

/* ---- for the self-test only ------------------------------------------
 *
 * Loopback never loses a segment, which makes it a poor place to find
 * out whether retransmission works - and "the retransmission path is
 * untested" is exactly the kind of thing that is true of most from-
 * scratch TCP implementations and is never written down. So the boot
 * self-test asks the stack to drop the next N segments it would have
 * sent, and then asserts the transfer completes anyway.
 *
 * Counts only segments carrying data, so the drop lands in go-back-N
 * rather than on the handshake - see the comment at the injection point
 * in emit() for why that distinction is the whole value of it.
 *
 * Deliberately not reachable from user space: it is a hole in the
 * network stack, and the fact that a self-test needs one is not a reason
 * for a program to have it. */
void tcp_debug_drop_next(int data_segments);
int tcp_debug_retransmits(void);

/* M116: segments discarded since boot because their checksum was wrong.
 * Not a debug hook - a count, readable by the boot self-test that moves a
 * real transfer through the real NIC and requires it to stay at zero. A
 * driver that hands the stack corrupt frames looks, from everywhere
 * else, exactly like a slow network. */
uint32_t tcp_checksum_failures(void);
