/* tests/test_unixsock.c - M118: AF_UNIX and SCM_RIGHTS, off the machine.
 *
 * kernel/ipc/unixsock.c is a ring buffer, a queue of records and a
 * refcount, and every interesting thing about it is a boundary: what
 * happens when a message is longer than the buffer, when the record queue
 * fills before the byte buffer does, when a descriptor arrives that the
 * receiver has no room for, when the peer dies with a message still
 * queued. A booted machine reaches none of those - two processes passing
 * a descriptor back and forth would pass against an implementation that
 * got every one of them wrong.
 *
 * ---- the instrument this file is really built around ------------------
 *
 * A passed descriptor is an fd_slot_t, and the reference counting on the
 * thing behind it is the part that cannot be eyeballed: one reference too
 * few and a process is left holding a freed pipe, one too many and
 * nothing on this machine ever closes. tests/fakes/fake_kernel_objects.c
 * counts exactly that - it is what Q13 built to grade the scheduler's
 * descriptor bookkeeping - so here "the receiver got a reference and the
 * sender's own is untouched" and "a message nobody read gave its
 * descriptors back" are assertions with a number in them rather than
 * arguments. Every test below ends at zero.
 */
#include "check.h"

#include "fakes/fakes.h"
#include "ipc/unixsock.h"
#include "sched/sched.h"

/* A descriptor to pass: a pipe read end, which the fake counts. The
 * pointer is never dereferenced by anything in this tier - the fake's
 * ref/unref take it and ignore it - so a distinct non-null value is
 * enough to tell two of them apart. */
static fd_slot_t a_pipe(int which) {
    fd_slot_t s;
    memset(&s, 0, sizeof(s));
    s.type = FD_PIPE_READ;
    s.pipe = (struct pipe *)(uintptr_t)(0x1000 + which * 0x10);
    return s;
}

static void clean(void) {
    unixsock_init();
    fake_objects_reset();
    CHECK_EQ(unixsock_in_use(), 0);
}

/* Every test ends here, and the two numbers are different claims: no
 * socket survived, and no descriptor any socket was carrying survived
 * either. */
static void expect_nothing_left(void) {
    CHECK_EQ(unixsock_in_use(), 0);
    CHECK_EQ(unixsock_queued_fds(), 0);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
}

TEST(unixsock, a_pair_carries_bytes_both_ways) {
    clean();
    struct unixsock *a = NULL;
    struct unixsock *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    CHECK_EQ(unixsock_in_use(), 2);

    uint8_t out[64];
    int nfds = 0, flags = 0;
    /* Nothing queued is 0 - "not yet" - and not -1, which is end of
     * stream. The two answers are the whole of why a reader can wait. */
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, &nfds, &flags), 0);

    CHECK_EQ(unixsock_send(a, (const uint8_t *)"hello", 5, NULL, 0), 5);
    CHECK_EQ(unixsock_pending(b), 1);
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, &nfds, &flags), 5);
    CHECK_MEMEQ(out, "hello", 5);
    CHECK_EQ(unixsock_pending(b), 0);

    CHECK_EQ(unixsock_send(b, (const uint8_t *)"back", 4, NULL, 0), 4);
    CHECK_EQ(unixsock_recv(a, out, sizeof(out), NULL, 0, &nfds, &flags), 4);
    CHECK_MEMEQ(out, "back", 4);

    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, a_stream_coalesces_and_a_message_does_not) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"ab", 2, NULL, 0), 2);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"cd", 2, NULL, 0), 2);
    uint8_t out[64];
    /* One read, both writes: that is what a byte stream means, and it is
     * also the coalescing that keeps a thousand small writes from
     * exhausting sixteen records. */
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), 4);
    CHECK_MEMEQ(out, "abcd", 4);
    unixsock_unref(a);
    unixsock_unref(b);

    struct unixsock *c = NULL, *d = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_SEQPACKET, &c, &d) == 0);
    CHECK_EQ(unixsock_send(c, (const uint8_t *)"ab", 2, NULL, 0), 2);
    CHECK_EQ(unixsock_send(c, (const uint8_t *)"cd", 2, NULL, 0), 2);
    CHECK_EQ(unixsock_recv(d, out, sizeof(out), NULL, 0, NULL, NULL), 2);
    CHECK_MEMEQ(out, "ab", 2);
    CHECK_EQ(unixsock_recv(d, out, sizeof(out), NULL, 0, NULL, NULL), 2);
    CHECK_MEMEQ(out, "cd", 2);
    unixsock_unref(c);
    unixsock_unref(d);
    expect_nothing_left();
}

TEST(unixsock, a_short_read_of_a_message_discards_the_rest) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_SEQPACKET, &a, &b) == 0);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"0123456789", 10, NULL, 0), 10);
    uint8_t out[4];
    int flags = 0;
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, &flags), 4);
    CHECK_MEMEQ(out, "0123", 4);
    CHECK_EQ(flags & UNIX_RECV_TRUNC, UNIX_RECV_TRUNC);
    /* And the remaining six bytes are GONE rather than waiting, which is
     * what POSIX requires of a message socket and the reason MSG_TRUNC
     * has to be reported rather than inferred. */
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), 0);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, a_full_buffer_is_a_wait_and_an_oversized_message_is_a_refusal) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    static uint8_t big[UNIX_BUF_SIZE + 64];
    memset(big, 'x', sizeof(big));
    /* A stream takes what fits and says how much - the short count every
     * caller loops on. */
    CHECK_EQ(unixsock_send(a, big, sizeof(big), NULL, 0), UNIX_BUF_SIZE);
    /* And then takes nothing, which is "would block" and not an error. */
    CHECK_EQ(unixsock_send(a, big, 1, NULL, 0), 0);
    uint8_t out[128];
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), 128);
    CHECK_EQ(unixsock_send(a, big, 1, NULL, 0), 1);
    unixsock_unref(a);
    unixsock_unref(b);

    struct unixsock *c = NULL, *d = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_SEQPACKET, &c, &d) == 0);
    /* A message that can never fit is refused rather than waited on:
     * waiting for space that will never be enough is a hang. */
    CHECK_EQ(unixsock_send(c, big, sizeof(big), NULL, 0), -1);
    CHECK_EQ(unixsock_send(c, big, UNIX_BUF_SIZE, NULL, 0), UNIX_BUF_SIZE);
    unixsock_unref(c);
    unixsock_unref(d);
    expect_nothing_left();
}

TEST(unixsock, the_record_queue_fills_before_the_buffer_does) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_SEQPACKET, &a, &b) == 0);
    for (int i = 0; i < UNIX_MAX_SEGS; i++) {
        CHECK_EQ(unixsock_send(a, (const uint8_t *)"m", 1, NULL, 0), 1);
    }
    /* Sixteen one-byte messages in a 4 KiB buffer: what ran out is the
     * record queue, and it has to read as "would block" rather than as an
     * error, or a sender loops forever on a refusal it cannot fix. */
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"m", 1, NULL, 0), 0);
    uint8_t out[8];
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), 1);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"m", 1, NULL, 0), 1);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, a_descriptor_crosses_and_its_refcount_is_exact) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    fd_slot_t send_me = a_pipe(1);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"fd", 2, &send_me, 1), 2);
    /* One reference taken by the send, held by the queue. The sender's own
     * descriptor is untouched - it may close it right now, which is what
     * every program that passes one does. */
    CHECK_EQ(fake_objects_pipe_read_refs(), 1);
    CHECK_EQ(unixsock_queued_fds(), 1);

    fd_slot_t got[UNIX_MAX_FDS];
    int nfds = 0, flags = 0;
    uint8_t out[16];
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, &flags), 2);
    CHECK_MEMEQ(out, "fd", 2);
    CHECK_EQ(nfds, 1);
    CHECK_EQ(flags, 0);
    CHECK_EQ(unixsock_queued_fds(), 0);
    /* Still exactly one: the reference moved, it was not duplicated. The
     * receiver owns it now and the socket no longer does. */
    CHECK_EQ(fake_objects_pipe_read_refs(), 1);
    CHECK(got[0].type == FD_PIPE_READ);
    CHECK(got[0].pipe == send_me.pipe);

    fd_release(&got[0]); /* what installing it into a table and closing it would do */
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, a_stream_read_stops_at_the_record_that_carries_descriptors) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    fd_slot_t one = a_pipe(1);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"AA", 2, NULL, 0), 2);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"BB", 2, &one, 1), 2);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"CC", 2, NULL, 0), 2);

    uint8_t out[64];
    fd_slot_t got[UNIX_MAX_FDS];
    int nfds = 0;
    /* Six bytes are queued and there is room for all six, but the read
     * stops after the first two: the next record carries a descriptor,
     * and the bytes that arrived with it have to be returned WITH it.
     * Every IPC layer in docs/browser.md depends on exactly this. */
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, NULL), 2);
    CHECK_MEMEQ(out, "AA", 2);
    CHECK_EQ(nfds, 0);

    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, NULL), 4);
    /* "BB" and then "CC": once the descriptors have been taken, the read
     * coalesces forward again. */
    CHECK_MEMEQ(out, "BBCC", 4);
    CHECK_EQ(nfds, 1);
    fd_release(&got[0]);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, descriptors_with_nowhere_to_go_are_closed_and_reported) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    fd_slot_t three[3] = {a_pipe(1), a_pipe(2), a_pipe(3)};
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"x", 1, three, 3), 1);
    CHECK_EQ(fake_objects_pipe_read_refs(), 3);

    uint8_t out[8];
    fd_slot_t got[1];
    int nfds = 0, flags = 0;
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, 1, &nfds, &flags), 1);
    CHECK_EQ(nfds, 1);
    CHECK_EQ(flags & UNIX_RECV_CTRUNC, UNIX_RECV_CTRUNC);
    /* The two that did not fit are closed here and now - not left queued
     * for a later recvmsg that would never be told they existed. */
    CHECK_EQ(fake_objects_pipe_read_refs(), 1);
    fd_release(&got[0]);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, a_read_that_cannot_carry_descriptors_drops_them) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    fd_slot_t one = a_pipe(1);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"y", 1, &one, 1), 1);
    uint8_t out[8];
    /* This is read(2) on the socket: no array to put a descriptor in. It
     * is closed, which is Linux's answer and the only one that does not
     * leave a reference nothing can release. */
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), 1);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, a_message_nobody_reads_gives_its_descriptors_back) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    fd_slot_t two[2] = {a_pipe(1), a_pipe(2)};
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"z", 1, two, 2), 1);
    CHECK_EQ(fake_objects_pipe_read_refs(), 2);
    /* The receiver dies with the message still queued, which is the
     * ordinary case for a crashed process and the one a leak hides in. */
    unixsock_unref(b);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    CHECK_EQ(unixsock_queued_fds(), 0);
    unixsock_unref(a);
    expect_nothing_left();
}

TEST(unixsock, a_socket_passed_over_a_socket_is_released_too) {
    clean();
    struct unixsock *a = NULL, *b = NULL, *c = NULL, *d = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &c, &d) == 0);
    CHECK_EQ(unixsock_in_use(), 4);
    /* Passing a channel over a channel is what Mojo does to make a second
     * one, and it is the case that found a deadlock in the first draft of
     * unixsock_send: fd_retain on this slot re-enters the file's own lock.
     * The test that proves the fix is this one - it would hang, not fail. */
    fd_slot_t pass;
    memset(&pass, 0, sizeof(pass));
    pass.type = FD_UNIX;
    pass.un = c;
    /* unixsock_pair's reference IS the sender's descriptor, so there is no
     * second one to take here - and getting that wrong is how this test
     * failed first time round, with one socket left alive at the end. */
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"s", 1, &pass, 1), 1);
    unixsock_unref(c); /* the sender closes its copy immediately, as every passer does */

    /* Nobody reads it; everything goes away. c survives until the queue
     * holding it does, and then does not survive. */
    unixsock_unref(a);
    unixsock_unref(d);
    CHECK_EQ(unixsock_in_use(), 2); /* b still holds the queued c */
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, bind_connect_accept_and_the_names_that_are_refused) {
    clean();
    struct unixsock *srv = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(srv != NULL);
    CHECK_EQ(unixsock_listen(srv), -1); /* not bound yet: a listener needs a name */
    CHECK_EQ(unixsock_bind(srv, "/tmp/s", 6), 0);
    CHECK_EQ(unixsock_bind(srv, "/tmp/other", 10), -1); /* one socket, one name */
    CHECK_EQ(unixsock_listen(srv), 0);

    struct unixsock *other = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(other != NULL);
    CHECK_EQ(unixsock_bind(other, "/tmp/s", 6), -1); /* EADDRINUSE */

    struct unixsock *cli = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(cli != NULL);
    CHECK_EQ(unixsock_connect(cli, "/tmp/nothing", 12), -1);
    CHECK_EQ(unixsock_connect(cli, "/tmp/s", 6), 0);
    CHECK_EQ(unixsock_pending(srv), 1);

    struct unixsock *conn = unixsock_accept(srv);
    REQUIRE(conn != NULL);
    CHECK_EQ(unixsock_pending(srv), 0);
    CHECK(unixsock_accept(srv) == NULL);

    /* The accepted socket is a DIFFERENT one from the listener, which is
     * what lets the listener take a second connection - and it is peered
     * with the client rather than with the name. */
    uint8_t out[16];
    CHECK_EQ(unixsock_send(cli, (const uint8_t *)"hi", 2, NULL, 0), 2);
    CHECK_EQ(unixsock_recv(conn, out, sizeof(out), NULL, 0, NULL, NULL), 2);
    CHECK_MEMEQ(out, "hi", 2);

    unixsock_unref(conn);
    unixsock_unref(cli);
    unixsock_unref(other);
    unixsock_unref(srv);
    /* And the name went with it: the next program may claim it. */
    struct unixsock *again = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(again != NULL);
    CHECK_EQ(unixsock_bind(again, "/tmp/s", 6), 0);
    unixsock_unref(again);
    expect_nothing_left();
}

TEST(unixsock, an_abstract_name_is_not_a_path_and_a_leading_nul_is_kept) {
    clean();
    struct unixsock *srv = unixsock_alloc(UNIX_SOCK_SEQPACKET);
    REQUIRE(srv != NULL);
    /* Two names that every string function in this kernel would call
     * equal, because both are the empty string: this is the whole reason
     * a bound name is bytes and a length rather than a char*. */
    const char abstract_a[] = {0, 'm', 'o', 'j', 'o'};
    const char abstract_b[] = {0, 'i', 'p', 'c'};
    CHECK_EQ(unixsock_bind(srv, abstract_a, 5), 0);
    CHECK_EQ(unixsock_listen(srv), 0);

    struct unixsock *cli = unixsock_alloc(UNIX_SOCK_SEQPACKET);
    REQUIRE(cli != NULL);
    CHECK_EQ(unixsock_connect(cli, abstract_b, 4), -1);
    CHECK_EQ(unixsock_connect(cli, abstract_a, 5), 0);
    struct unixsock *conn = unixsock_accept(srv);
    REQUIRE(conn != NULL);
    unixsock_unref(conn);
    unixsock_unref(cli);
    unixsock_unref(srv);
    expect_nothing_left();
}

TEST(unixsock, a_connect_of_the_wrong_type_is_refused) {
    clean();
    struct unixsock *srv = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(srv != NULL);
    CHECK_EQ(unixsock_bind(srv, "/tmp/t", 6), 0);
    CHECK_EQ(unixsock_listen(srv), 0);
    struct unixsock *cli = unixsock_alloc(UNIX_SOCK_SEQPACKET);
    REQUIRE(cli != NULL);
    /* A message socket connected to a byte-stream listener would give
     * both sides a channel whose boundaries only one of them believes in. */
    CHECK_EQ(unixsock_connect(cli, "/tmp/t", 6), -1);
    unixsock_unref(cli);
    unixsock_unref(srv);
    expect_nothing_left();
}

TEST(unixsock, a_full_backlog_refuses_and_recovers) {
    clean();
    struct unixsock *srv = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(srv != NULL);
    CHECK_EQ(unixsock_bind(srv, "/tmp/b", 6), 0);
    CHECK_EQ(unixsock_listen(srv), 0);
    struct unixsock *clients[UNIX_BACKLOG + 1];
    for (int i = 0; i < UNIX_BACKLOG + 1; i++) {
        clients[i] = unixsock_alloc(UNIX_SOCK_STREAM);
        REQUIRE(clients[i] != NULL);
        long rc = unixsock_connect(clients[i], "/tmp/b", 6);
        CHECK_EQ(rc, i < UNIX_BACKLOG ? 0 : -1);
    }
    /* Q9's rule, on a new table: a resource that refuses when full and
     * never works again passes an exhaustion test and is broken. */
    struct unixsock *conn = unixsock_accept(srv);
    REQUIRE(conn != NULL);
    CHECK_EQ(unixsock_connect(clients[UNIX_BACKLOG], "/tmp/b", 6), 0);
    unixsock_unref(conn);
    for (int i = 0; i < UNIX_BACKLOG + 1; i++) {
        unixsock_unref(clients[i]);
    }
    unixsock_unref(srv);
    expect_nothing_left();
}

TEST(unixsock, a_peer_that_goes_away_is_end_of_stream_and_then_EPIPE) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"last", 4, NULL, 0), 4);
    unixsock_unref(a);
    uint8_t out[16];
    /* The bytes already sent are still there - a reader does not lose
     * data because the writer exited - and only once they are drained
     * does the read report the end. */
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), 4);
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), -1);
    CHECK_EQ(unixsock_pending(b), 1); /* and the end of stream is READABLE, or a waiter hangs on it */
    CHECK_EQ(unixsock_send(b, (const uint8_t *)"?", 1, NULL, 0), -1);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, shutdown_says_I_am_finished_without_closing) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"bye", 3, NULL, 0), 3);
    CHECK_EQ(unixsock_shutdown(a, 1), 0); /* SHUT_WR */
    uint8_t out[16];
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), 3);
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), -1);
    /* And the other direction still works, which is the entire point of a
     * half-close: the descriptor is open, the peer can still answer. */
    CHECK_EQ(unixsock_send(b, (const uint8_t *)"ok", 2, NULL, 0), 2);
    CHECK_EQ(unixsock_recv(a, out, sizeof(out), NULL, 0, NULL, NULL), 2);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"?", 1, NULL, 0), -1); /* this side promised not to */
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, shut_rd_makes_the_senders_writes_fail) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_SEQPACKET, &a, &b) == 0);
    CHECK_EQ(unixsock_shutdown(b, 0), 0); /* SHUT_RD on the receiver */
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"x", 1, NULL, 0), -1);
    CHECK_EQ(unixsock_recv(b, NULL, 0, NULL, 0, NULL, NULL), -1);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, running_out_of_sockets_refuses_and_recovers) {
    clean();
    struct unixsock *all[UNIX_MAX_SOCKETS];
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) {
        all[i] = unixsock_alloc(UNIX_SOCK_STREAM);
        REQUIRE(all[i] != NULL);
    }
    CHECK(unixsock_alloc(UNIX_SOCK_STREAM) == NULL);
    /* A pair needs two and takes neither when it cannot have both - the
     * case that would otherwise leave a socket whose peer nothing can
     * name. */
    struct unixsock *x = NULL, *y = NULL;
    unixsock_unref(all[0]);
    CHECK_EQ(unixsock_pair(UNIX_SOCK_STREAM, &x, &y), -1);
    CHECK_EQ(unixsock_in_use(), UNIX_MAX_SOCKETS - 1);
    for (int i = 1; i < UNIX_MAX_SOCKETS; i++) {
        unixsock_unref(all[i]);
    }
    CHECK_EQ(unixsock_pair(UNIX_SOCK_STREAM, &x, &y), 0);
    unixsock_unref(x);
    unixsock_unref(y);
    expect_nothing_left();
}

TEST(unixsock, the_name_table_refuses_and_recovers) {
    clean();
    struct unixsock *s[UNIX_MAX_NAMES + 1];
    for (int i = 0; i < UNIX_MAX_NAMES + 1; i++) {
        s[i] = unixsock_alloc(UNIX_SOCK_STREAM);
        REQUIRE(s[i] != NULL);
        char name[8] = {'/', 'n', (char)('0' + i % 10), (char)('a' + i / 10), 0};
        CHECK_EQ(unixsock_bind(s[i], name, 4), i < UNIX_MAX_NAMES ? 0 : -1);
    }
    for (int i = 0; i < UNIX_MAX_NAMES + 1; i++) {
        unixsock_unref(s[i]);
    }
    struct unixsock *again = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(again != NULL);
    CHECK_EQ(unixsock_bind(again, "/n0a", 4), 0);
    unixsock_unref(again);
    expect_nothing_left();
}

TEST(unixsock, a_zero_length_message_carrying_a_descriptor_is_a_message) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    fd_slot_t one = a_pipe(1);
    /* Handing over a handle with no payload. It has to be deliverable:
     * an IPC layer that must send a byte to send a descriptor is an IPC
     * layer with a protocol quirk nobody asked for. */
    CHECK_EQ(unixsock_send(a, NULL, 0, &one, 1), 0);
    CHECK_EQ(unixsock_pending(b), 1);
    fd_slot_t got[UNIX_MAX_FDS];
    int nfds = 0;
    uint8_t out[8];
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, NULL), 0);
    CHECK_EQ(nfds, 1);
    fd_release(&got[0]);
    /* And a zero-length send with nothing attached is a no-op rather than
     * a record, which is what write(fd, buf, 0) means. */
    CHECK_EQ(unixsock_send(a, NULL, 0, NULL, 0), 0);
    CHECK_EQ(unixsock_pending(b), 0);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, too_many_descriptors_is_refused_before_anything_moves) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    fd_slot_t many[UNIX_MAX_FDS + 1];
    for (int i = 0; i < UNIX_MAX_FDS + 1; i++) {
        many[i] = a_pipe(i);
    }
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"x", 1, many, UNIX_MAX_FDS + 1), -1);
    /* Nothing was retained and no byte was queued: a refusal that had
     * already taken references would leak every one of them. */
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    CHECK_EQ(unixsock_pending(b), 0);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

/* ---- the mutation harness's findings ---------------------------------
 *
 * Everything above was written from the header's claims. `make mutate
 * FILE=kernel/ipc/unixsock.c` then broke the file on purpose forty times
 * and fifteen of those went unnoticed, which is the only instrument here
 * that grades the tests rather than the code (Q8's own lesson: the first
 * file it examined had full line coverage and a mutation score of zero).
 * Each test below was written against a specific survivor, and each one
 * is a claim the header already made and nothing checked.
 */

TEST(unixsock, a_type_that_does_not_exist_is_refused) {
    clean();
    /* AF_UNIX SOCK_DGRAM is the one a program is most likely to ask for -
     * it is what <sys/socket.h>'s SOCK_DGRAM is - and it is not built.
     * Refused, rather than quietly given a stream whose boundaries the
     * caller does not expect. */
    CHECK(unixsock_alloc(2) == NULL);
    CHECK(unixsock_alloc(0) == NULL);
    CHECK(unixsock_alloc(99) == NULL);
    struct unixsock *x = NULL, *y = NULL;
    CHECK_EQ(unixsock_pair(2, &x, &y), -1);
    CHECK(x == NULL && y == NULL);
    CHECK_EQ(unixsock_in_use(), 0); /* and a refusal allocates nothing */
    expect_nothing_left();
}

TEST(unixsock, a_socket_reports_its_own_type) {
    clean();
    struct unixsock *s = unixsock_alloc(UNIX_SOCK_SEQPACKET);
    REQUIRE(s != NULL);
    /* The syscall layer asks this to decide whether an oversized send is
     * a short write or an EMSGSIZE, so a wrong answer here is a silently
     * truncated message. */
    CHECK_EQ(unixsock_type(s), UNIX_SOCK_SEQPACKET);
    unixsock_unref(s);
    struct unixsock *t = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(t != NULL);
    CHECK_EQ(unixsock_type(t), UNIX_SOCK_STREAM);
    unixsock_unref(t);
    CHECK_EQ(unixsock_type(NULL), -1);
    expect_nothing_left();
}

TEST(unixsock, a_name_of_every_legal_length_and_none_that_is_not) {
    clean();
    struct unixsock *s = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(s != NULL);
    CHECK_EQ(unixsock_bind(s, "x", 0), -1);                  /* no name at all */
    CHECK_EQ(unixsock_bind(s, "x", -1), -1);
    CHECK_EQ(unixsock_bind(s, NULL, 4), -1);
    char full[UNIX_PATH_MAX + 1];
    memset(full, 'n', sizeof(full));
    CHECK_EQ(unixsock_bind(s, full, UNIX_PATH_MAX + 1), -1); /* one byte too long */
    /* And the boundary itself, which is the length sizeof(sun_path) allows
     * and the one an off-by-one in the copy would run past - under ASan,
     * that is a report rather than a guess. */
    CHECK_EQ(unixsock_bind(s, full, UNIX_PATH_MAX), 0);
    struct unixsock *c = unixsock_alloc(UNIX_SOCK_STREAM);
    REQUIRE(c != NULL);
    CHECK_EQ(unixsock_listen(s), 0);
    CHECK_EQ(unixsock_connect(c, full, UNIX_PATH_MAX), 0);
    CHECK_EQ(unixsock_connect(c, full, 0), -1);
    CHECK_EQ(unixsock_connect(c, full, UNIX_PATH_MAX + 1), -1);
    struct unixsock *conn = unixsock_accept(s);
    REQUIRE(conn != NULL);
    unixsock_unref(conn);
    unixsock_unref(c);
    unixsock_unref(s);
    expect_nothing_left();
}

TEST(unixsock, a_message_that_does_not_fit_right_now_waits_whole) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_SEQPACKET, &a, &b) == 0);
    static uint8_t buf[UNIX_BUF_SIZE];
    memset(buf, 'a', sizeof(buf));
    CHECK_EQ(unixsock_send(a, buf, UNIX_BUF_SIZE - 8, NULL, 0), UNIX_BUF_SIZE - 8);
    /* Sixteen bytes offered into eight bytes of space. A stream would take
     * eight; a message socket must take NONE - a receiver that got eight
     * bytes of a sixteen-byte message would have no way to know. */
    CHECK_EQ(unixsock_send(a, buf, 16, NULL, 0), 0);
    uint8_t out[UNIX_BUF_SIZE];
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), UNIX_BUF_SIZE - 8);
    CHECK_EQ(unixsock_send(a, buf, 16, NULL, 0), 16);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, a_stream_of_small_writes_does_not_exhaust_the_record_queue) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    /* Four times as many writes as there are records. Coalescing is what
     * makes this work, and without it a program doing putc() into a socket
     * would stall after sixteen bytes - which is the bug this asserts
     * against, not a property of the arithmetic. */
    for (int i = 0; i < UNIX_MAX_SEGS * 4; i++) {
        CHECK_EQ(unixsock_send(a, (const uint8_t *)"s", 1, NULL, 0), 1);
    }
    uint8_t out[UNIX_MAX_SEGS * 4 + 8];
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), UNIX_MAX_SEGS * 4);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, recv_clears_what_it_is_about_to_report) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"plain", 5, NULL, 0), 5);
    uint8_t out[16];
    fd_slot_t got[UNIX_MAX_FDS];
    /* A caller's variables, deliberately dirty. A recv that returns bytes
     * and no descriptors has to SAY no descriptors - leaving the previous
     * call's count there would have the caller install a descriptor number
     * it was never given. */
    int nfds = 7;
    int flags = 0xFF;
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, &flags), 5);
    CHECK_EQ(nfds, 0);
    CHECK_EQ(flags, 0);
    /* And the same on the paths that return nothing at all, which is where
     * a caller is least likely to look. */
    nfds = 7;
    flags = 0xFF;
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, &flags), 0);
    CHECK_EQ(nfds, 0);
    CHECK_EQ(flags, 0);
    unixsock_unref(a);
    nfds = 7;
    flags = 0xFF;
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, &flags), -1);
    CHECK_EQ(nfds, 0);
    CHECK_EQ(flags, 0);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, descriptors_are_delivered_once_even_when_the_record_survives) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    fd_slot_t one = a_pipe(1);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"0123456789", 10, &one, 1), 10);
    uint8_t out[4];
    fd_slot_t got[UNIX_MAX_FDS];
    int nfds = 0;
    /* Four bytes of a ten-byte record, so the record is still there
     * afterwards with its bytes and - this is the claim - without its
     * descriptor. Handing it over twice would give the receiver two
     * references to something the sender granted once. */
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, NULL), 4);
    CHECK_EQ(nfds, 1);
    fd_release(&got[0]);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    nfds = 0;
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), got, UNIX_MAX_FDS, &nfds, NULL), 4);
    CHECK_EQ(nfds, 0);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, a_truncated_message_does_not_take_the_next_one_with_it) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_SEQPACKET, &a, &b) == 0);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"first-long", 10, NULL, 0), 10);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"second", 6, NULL, 0), 6);
    uint8_t out[8];
    int flags = 0;
    CHECK_EQ(unixsock_recv(b, out, 4, NULL, 0, NULL, &flags), 4);
    CHECK_MEMEQ(out, "firs", 4);
    CHECK_EQ(flags & UNIX_RECV_TRUNC, UNIX_RECV_TRUNC);
    /* The second message is intact and next. Discarding the first one's
     * tail must discard exactly that - a queue that skipped a record here
     * would lose a whole message and look like a dropped packet. */
    CHECK_EQ(unixsock_recv(b, out, sizeof(out), NULL, 0, NULL, NULL), 6);
    CHECK_MEMEQ(out, "second", 6);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}

TEST(unixsock, shutdown_refuses_a_how_it_does_not_have) {
    clean();
    struct unixsock *a = NULL, *b = NULL;
    REQUIRE(unixsock_pair(UNIX_SOCK_STREAM, &a, &b) == 0);
    CHECK_EQ(unixsock_shutdown(a, 3), -1);
    CHECK_EQ(unixsock_shutdown(a, -1), -1);
    CHECK_EQ(unixsock_shutdown(NULL, 1), -1);
    /* And SHUT_RDWR does both halves, which is the one of the three that
     * is two claims rather than one. */
    CHECK_EQ(unixsock_shutdown(a, 2), 0);
    CHECK_EQ(unixsock_send(a, (const uint8_t *)"x", 1, NULL, 0), -1);
    uint8_t out[8];
    CHECK_EQ(unixsock_recv(a, out, sizeof(out), NULL, 0, NULL, NULL), -1);
    unixsock_unref(a);
    unixsock_unref(b);
    expect_nothing_left();
}
