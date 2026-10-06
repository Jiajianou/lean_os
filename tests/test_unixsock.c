#include "check.h"

#include "fakes/fakes.h"
#include "inter_process_communication/unix_socket.h"
#include "scheduler/scheduler.h"

static file_descriptor_slot_t a_pipe(int which) {
    file_descriptor_slot_t s;
    memset(&s, 0, sizeof(s));
    s.type = FILE_DESCRIPTOR_PIPE_READ;
    s.pipe = (struct pipe *)(uintptr_t)(0x1000 + which * 0x10);
    return s;
}

static void clean(void) {
    unix_socket_init();
    fake_objects_reset();
    CHECK_EQ(unix_socket_in_use(), 0);
}

static void expect_nothing_left(void) {
    CHECK_EQ(unix_socket_in_use(), 0);
    CHECK_EQ(unix_socket_queued_file_descriptors(), 0);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
}

TEST(unix_socket, a_pair_carries_bytes_both_ways) {
    clean();
    struct unix_socket *a = NULL;
    struct unix_socket *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    CHECK_EQ(unix_socket_in_use(), 2);

    uint8_t out[64];
    int nfds = 0, flags = 0;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, &nfds, &flags), 0);

    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"hello", 5, NULL, 0), 5);
    CHECK_EQ(unix_socket_pending(b), 1);
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, &nfds, &flags), 5);
    CHECK_MEMEQ(out, "hello", 5);
    CHECK_EQ(unix_socket_pending(b), 0);

    CHECK_EQ(unix_socket_send(b, (const uint8_t *)"back", 4, NULL, 0), 4);
    CHECK_EQ(unix_socket_receive(a, out, sizeof(out), NULL, 0, &nfds, &flags), 4);
    CHECK_MEMEQ(out, "back", 4);

    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

/* The AF_UNIX half of MSG_PEEK, and the same defect M183 found on TCP: a
   peek that consumes is not a peek. Descriptors are deliberately NOT
   reported by one - handing the same descriptor over twice would be two
   references to one object where the sender sent one - so what a peek shows
   is the bytes, and the descriptors are still there for the receive after
   it. */
TEST(unix_socket, a_peek_shows_the_bytes_and_takes_nothing) {
    clean();
    struct unix_socket *a = NULL;
    struct unix_socket *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);

    uint8_t out[64];
    memset(out, 0, sizeof(out));
    CHECK_EQ(unix_socket_peek(b, out, sizeof(out)), 0);

    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"hello", 5, NULL, 0), 5);
    uint8_t one = 0;
    CHECK_EQ(unix_socket_peek(b, &one, 1), 1);
    CHECK_EQ(one, 'h');
    CHECK_EQ(unix_socket_readable_bytes(b), 5);

    CHECK_EQ(unix_socket_peek(b, out, sizeof(out)), 5);
    CHECK_MEMEQ(out, "hello", 5);
    CHECK_EQ(unix_socket_readable_bytes(b), 5);

    int nfds = 0, flags = 0;
    memset(out, 0, sizeof(out));
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, &nfds, &flags), 5);
    CHECK_MEMEQ(out, "hello", 5);
    CHECK_EQ(unix_socket_readable_bytes(b), 0);

    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

/* A peek at a socket whose peer has gone is end of file, and a peek at a
   live one with nothing on it is "would block" - the two different nothings
   recv(2) has to be able to tell apart. */
/* A datagram is the unit on a SEQPACKET socket, so a peek shows the message
   at the head of the queue and not the one behind it - the same rule the
   receive follows, and the reason a peek cannot just report s->count. */
TEST(unix_socket, a_peek_at_a_message_socket_stops_at_the_first_message) {
    clean();
    struct unix_socket *a = NULL;
    struct unix_socket *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &a, &b) == 0);

    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"one", 3, NULL, 0), 3);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"two", 3, NULL, 0), 3);

    uint8_t out[64];
    memset(out, 0, sizeof(out));
    CHECK_EQ(unix_socket_peek(b, out, sizeof(out)), 3);
    CHECK_MEMEQ(out, "one", 3);
    /* Three reported out of six queued: the peek stopped at the message
       boundary and took neither message. */
    CHECK_EQ(unix_socket_readable_bytes(b), 6);

    int nfds = 0, flags = 0;
    memset(out, 0, sizeof(out));
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, &nfds, &flags), 3);
    CHECK_MEMEQ(out, "one", 3);
    memset(out, 0, sizeof(out));
    CHECK_EQ(unix_socket_peek(b, out, sizeof(out)), 3);
    CHECK_MEMEQ(out, "two", 3);

    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

/* A socket shut down for reading refuses a peek rather than answering out of
   a queue nobody may take from. */
TEST(unix_socket, a_peek_at_a_socket_shut_for_reading_is_refused) {
    clean();
    struct unix_socket *a = NULL;
    struct unix_socket *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);

    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"hello", 5, NULL, 0), 5);
    uint8_t one = 0;
    CHECK_EQ(unix_socket_peek(b, &one, 1), 1);
    CHECK_EQ(unix_socket_shutdown(b, 0), 0);
    CHECK_EQ(unix_socket_peek(b, &one, 1), -1);
    CHECK_EQ(unix_socket_peek(NULL, &one, 1), -1);

    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_peek_tells_an_empty_socket_from_a_finished_one) {
    clean();
    struct unix_socket *a = NULL;
    struct unix_socket *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);

    uint8_t one = 0;
    CHECK_EQ(unix_socket_peek(b, &one, 1), 0);
    CHECK_EQ(unix_socket_shutdown(a, 1), 0);
    CHECK_EQ(unix_socket_peek(b, &one, 1), -1);

    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_stream_coalesces_and_a_message_does_not) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"ab", 2, NULL, 0), 2);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"cd", 2, NULL, 0), 2);
    uint8_t out[64];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 4);
    CHECK_MEMEQ(out, "abcd", 4);
    unix_socket_unref(a);
    unix_socket_unref(b);

    struct unix_socket *c = NULL, *d = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &c, &d) == 0);
    CHECK_EQ(unix_socket_send(c, (const uint8_t *)"ab", 2, NULL, 0), 2);
    CHECK_EQ(unix_socket_send(c, (const uint8_t *)"cd", 2, NULL, 0), 2);
    CHECK_EQ(unix_socket_receive(d, out, sizeof(out), NULL, 0, NULL, NULL), 2);
    CHECK_MEMEQ(out, "ab", 2);
    CHECK_EQ(unix_socket_receive(d, out, sizeof(out), NULL, 0, NULL, NULL), 2);
    CHECK_MEMEQ(out, "cd", 2);
    unix_socket_unref(c);
    unix_socket_unref(d);
    expect_nothing_left();
}

TEST(unix_socket, a_short_read_of_a_message_discards_the_rest) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &a, &b) == 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"0123456789", 10, NULL, 0), 10);
    uint8_t out[4];
    int flags = 0;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, &flags), 4);
    CHECK_MEMEQ(out, "0123", 4);
    CHECK_EQ(flags & UNIX_RECEIVE_TRUNC, UNIX_RECEIVE_TRUNC);
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 0);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_full_buffer_is_a_wait_and_an_oversized_message_is_a_refusal) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    static uint8_t big[UNIX_BUFFER_SIZE + 64];
    memset(big, 'x', sizeof(big));
    CHECK_EQ(unix_socket_send(a, big, sizeof(big), NULL, 0), UNIX_BUFFER_SIZE);
    CHECK_EQ(unix_socket_send(a, big, 1, NULL, 0), 0);
    uint8_t out[128];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 128);
    CHECK_EQ(unix_socket_send(a, big, 1, NULL, 0), 1);
    unix_socket_unref(a);
    unix_socket_unref(b);

    struct unix_socket *c = NULL, *d = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &c, &d) == 0);
    CHECK_EQ(unix_socket_send(c, big, sizeof(big), NULL, 0), -1);
    CHECK_EQ(unix_socket_send(c, big, UNIX_BUFFER_SIZE, NULL, 0), UNIX_BUFFER_SIZE);
    unix_socket_unref(c);
    unix_socket_unref(d);
    expect_nothing_left();
}

TEST(unix_socket, the_record_queue_fills_before_the_buffer_does) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &a, &b) == 0);
    for (int i = 0; i < UNIX_MAX_SEGS; i++) {
        CHECK_EQ(unix_socket_send(a, (const uint8_t *)"m", 1, NULL, 0), 1);
    }
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"m", 1, NULL, 0), 0);
    uint8_t out[8];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 1);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"m", 1, NULL, 0), 1);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_descriptor_crosses_and_its_refcount_is_exact) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t send_me = a_pipe(1);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"fd", 2, &send_me, 1), 2);
    CHECK_EQ(fake_objects_pipe_read_refs(), 1);
    CHECK_EQ(unix_socket_queued_file_descriptors(), 1);

    file_descriptor_slot_t got[UNIX_MAX_FILE_DESCRIPTORS];
    int nfds = 0, flags = 0;
    uint8_t out[16];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, &flags), 2);
    CHECK_MEMEQ(out, "fd", 2);
    CHECK_EQ(nfds, 1);
    CHECK_EQ(flags, 0);
    CHECK_EQ(unix_socket_queued_file_descriptors(), 0);
    CHECK_EQ(fake_objects_pipe_read_refs(), 1);
    CHECK(got[0].type == FILE_DESCRIPTOR_PIPE_READ);
    CHECK(got[0].pipe == send_me.pipe);

    file_descriptor_release(&got[0]);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_stream_read_stops_at_the_record_that_carries_descriptors) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t one = a_pipe(1);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"AA", 2, NULL, 0), 2);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"BB", 2, &one, 1), 2);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"CC", 2, NULL, 0), 2);

    uint8_t out[64];
    file_descriptor_slot_t got[UNIX_MAX_FILE_DESCRIPTORS];
    int nfds = 0;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, NULL), 2);
    CHECK_MEMEQ(out, "AA", 2);
    CHECK_EQ(nfds, 0);

    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, NULL), 4);
    CHECK_MEMEQ(out, "BBCC", 4);
    CHECK_EQ(nfds, 1);
    file_descriptor_release(&got[0]);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, descriptors_with_nowhere_to_go_are_closed_and_reported) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t three[3] = {a_pipe(1), a_pipe(2), a_pipe(3)};
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"x", 1, three, 3), 1);
    CHECK_EQ(fake_objects_pipe_read_refs(), 3);

    uint8_t out[8];
    file_descriptor_slot_t got[1];
    int nfds = 0, flags = 0;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, 1, &nfds, &flags), 1);
    CHECK_EQ(nfds, 1);
    CHECK_EQ(flags & UNIX_RECEIVE_CTRUNC, UNIX_RECEIVE_CTRUNC);
    CHECK_EQ(fake_objects_pipe_read_refs(), 1);
    file_descriptor_release(&got[0]);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_read_that_cannot_carry_descriptors_drops_them) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t one = a_pipe(1);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"y", 1, &one, 1), 1);
    uint8_t out[8];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 1);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_message_nobody_reads_gives_its_descriptors_back) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t two[2] = {a_pipe(1), a_pipe(2)};
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"z", 1, two, 2), 1);
    CHECK_EQ(fake_objects_pipe_read_refs(), 2);
    unix_socket_unref(b);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    CHECK_EQ(unix_socket_queued_file_descriptors(), 0);
    unix_socket_unref(a);
    expect_nothing_left();
}

TEST(unix_socket, a_socket_passed_over_a_socket_is_released_too) {
    clean();
    struct unix_socket *a = NULL, *b = NULL, *c = NULL, *d = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &c, &d) == 0);
    CHECK_EQ(unix_socket_in_use(), 4);
    file_descriptor_slot_t pass;
    memset(&pass, 0, sizeof(pass));
    pass.type = FILE_DESCRIPTOR_UNIX;
    pass.un = c;
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"s", 1, &pass, 1), 1);
    unix_socket_unref(c);

    unix_socket_unref(a);
    unix_socket_unref(d);
    CHECK_EQ(unix_socket_in_use(), 2);
    unix_socket_unref(b);
    expect_nothing_left();
}

/* M187: a listener is ready only when a connection is queued. It has no peer
   and never will, and reporting that as a hang-up made every listener look
   ready forever - which Chromium's ProcessSingleton answers with a blocking
   accept(), and the browser's IO thread never came back. */
TEST(unix_socket, a_listener_reports_nothing_until_a_connection_is_queued) {
    clean();
    struct unix_socket *srv = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(srv != NULL);
    CHECK_EQ(unix_socket_bind(srv, "/tmp/singleton", 14), 0);
    CHECK_EQ(unix_socket_listen(srv), 0);

    CHECK_EQ(unix_socket_pending(srv), 0);
    CHECK_EQ(unix_socket_hup(srv), 0);
    CHECK_EQ(unix_socket_rdhup(srv), 0);
    CHECK_EQ(unix_socket_writable(srv), 0);

    struct unix_socket *cli = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(cli != NULL);
    CHECK_EQ(unix_socket_connect(cli, "/tmp/singleton", 14), 0);
    CHECK_EQ(unix_socket_pending(srv), 1);
    CHECK_EQ(unix_socket_hup(srv), 0);

    struct unix_socket *conn = unix_socket_accept(srv);
    REQUIRE(conn != NULL);
    CHECK_EQ(unix_socket_pending(srv), 0);
    CHECK_EQ(unix_socket_hup(srv), 0);

    /* An unconnected, non-listening socket is still a hang-up, as Linux's
       is. */
    struct unix_socket *lonely = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(lonely != NULL);
    CHECK_EQ(unix_socket_hup(lonely), 1);

    unix_socket_unref(lonely);
    unix_socket_unref(conn);
    unix_socket_unref(cli);
    unix_socket_unref(srv);
    expect_nothing_left();
}

TEST(unix_socket, bind_connect_accept_and_the_names_that_are_refused) {
    clean();
    struct unix_socket *srv = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(srv != NULL);
    CHECK_EQ(unix_socket_listen(srv), -1);
    CHECK_EQ(unix_socket_bind(srv, "/tmp/s", 6), 0);
    CHECK_EQ(unix_socket_bind(srv, "/tmp/other", 10), -1);
    CHECK_EQ(unix_socket_listen(srv), 0);

    struct unix_socket *other = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(other != NULL);
    CHECK_EQ(unix_socket_bind(other, "/tmp/s", 6), -1);

    struct unix_socket *cli = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(cli != NULL);
    CHECK_EQ(unix_socket_connect(cli, "/tmp/nothing", 12), -1);
    CHECK_EQ(unix_socket_connect(cli, "/tmp/s", 6), 0);
    CHECK_EQ(unix_socket_pending(srv), 1);

    struct unix_socket *conn = unix_socket_accept(srv);
    REQUIRE(conn != NULL);
    CHECK_EQ(unix_socket_pending(srv), 0);
    CHECK(unix_socket_accept(srv) == NULL);

    uint8_t out[16];
    CHECK_EQ(unix_socket_send(cli, (const uint8_t *)"hi", 2, NULL, 0), 2);
    CHECK_EQ(unix_socket_receive(conn, out, sizeof(out), NULL, 0, NULL, NULL), 2);
    CHECK_MEMEQ(out, "hi", 2);

    unix_socket_unref(conn);
    unix_socket_unref(cli);
    unix_socket_unref(other);
    unix_socket_unref(srv);
    struct unix_socket *again = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(again != NULL);
    CHECK_EQ(unix_socket_bind(again, "/tmp/s", 6), 0);
    unix_socket_unref(again);
    expect_nothing_left();
}

TEST(unix_socket, an_abstract_name_is_not_a_path_and_a_leading_nul_is_kept) {
    clean();
    struct unix_socket *srv = unix_socket_alloc(UNIX_SOCKET_SEQPACKET);
    REQUIRE(srv != NULL);
    const char abstract_a[] = {0, 'm', 'o', 'j', 'o'};
    const char abstract_b[] = {0, 'i', 'p', 'c'};
    CHECK_EQ(unix_socket_bind(srv, abstract_a, 5), 0);
    CHECK_EQ(unix_socket_listen(srv), 0);

    struct unix_socket *cli = unix_socket_alloc(UNIX_SOCKET_SEQPACKET);
    REQUIRE(cli != NULL);
    CHECK_EQ(unix_socket_connect(cli, abstract_b, 4), -1);
    CHECK_EQ(unix_socket_connect(cli, abstract_a, 5), 0);
    struct unix_socket *conn = unix_socket_accept(srv);
    REQUIRE(conn != NULL);
    unix_socket_unref(conn);
    unix_socket_unref(cli);
    unix_socket_unref(srv);
    expect_nothing_left();
}

TEST(unix_socket, a_connect_of_the_wrong_type_is_refused) {
    clean();
    struct unix_socket *srv = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(srv != NULL);
    CHECK_EQ(unix_socket_bind(srv, "/tmp/t", 6), 0);
    CHECK_EQ(unix_socket_listen(srv), 0);
    struct unix_socket *cli = unix_socket_alloc(UNIX_SOCKET_SEQPACKET);
    REQUIRE(cli != NULL);
    CHECK_EQ(unix_socket_connect(cli, "/tmp/t", 6), -1);
    unix_socket_unref(cli);
    unix_socket_unref(srv);
    expect_nothing_left();
}

TEST(unix_socket, a_full_backlog_refuses_and_recovers) {
    clean();
    struct unix_socket *srv = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(srv != NULL);
    CHECK_EQ(unix_socket_bind(srv, "/tmp/b", 6), 0);
    CHECK_EQ(unix_socket_listen(srv), 0);
    struct unix_socket *clients[UNIX_BACKLOG + 1];
    for (int i = 0; i < UNIX_BACKLOG + 1; i++) {
        clients[i] = unix_socket_alloc(UNIX_SOCKET_STREAM);
        REQUIRE(clients[i] != NULL);
        long rc = unix_socket_connect(clients[i], "/tmp/b", 6);
        CHECK_EQ(rc, i < UNIX_BACKLOG ? 0 : -1);
    }
    struct unix_socket *conn = unix_socket_accept(srv);
    REQUIRE(conn != NULL);
    CHECK_EQ(unix_socket_connect(clients[UNIX_BACKLOG], "/tmp/b", 6), 0);
    unix_socket_unref(conn);
    for (int i = 0; i < UNIX_BACKLOG + 1; i++) {
        unix_socket_unref(clients[i]);
    }
    unix_socket_unref(srv);
    expect_nothing_left();
}

TEST(unix_socket, a_peer_that_goes_away_is_end_of_stream_and_then_EPIPE) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"last", 4, NULL, 0), 4);
    unix_socket_unref(a);
    uint8_t out[16];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 4);
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), -1);
    CHECK_EQ(unix_socket_pending(b), 1);
    CHECK_EQ(unix_socket_send(b, (const uint8_t *)"?", 1, NULL, 0), -1);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, shutdown_says_I_am_finished_without_closing) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"bye", 3, NULL, 0), 3);
    CHECK_EQ(unix_socket_shutdown(a, 1), 0);
    uint8_t out[16];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 3);
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), -1);
    CHECK_EQ(unix_socket_send(b, (const uint8_t *)"ok", 2, NULL, 0), 2);
    CHECK_EQ(unix_socket_receive(a, out, sizeof(out), NULL, 0, NULL, NULL), 2);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"?", 1, NULL, 0), -1);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, shut_rd_makes_the_senders_writes_fail) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &a, &b) == 0);
    CHECK_EQ(unix_socket_shutdown(b, 0), 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"x", 1, NULL, 0), -1);
    CHECK_EQ(unix_socket_receive(b, NULL, 0, NULL, 0, NULL, NULL), -1);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, running_out_of_sockets_refuses_and_recovers) {
    clean();
    struct unix_socket *all[UNIX_MAX_SOCKETS];
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) {
        all[i] = unix_socket_alloc(UNIX_SOCKET_STREAM);
        REQUIRE(all[i] != NULL);
    }
    CHECK(unix_socket_alloc(UNIX_SOCKET_STREAM) == NULL);
    struct unix_socket *x = NULL, *y = NULL;
    unix_socket_unref(all[0]);
    CHECK_EQ(unix_socket_pair(UNIX_SOCKET_STREAM, &x, &y), -1);
    CHECK_EQ(unix_socket_in_use(), UNIX_MAX_SOCKETS - 1);
    for (int i = 1; i < UNIX_MAX_SOCKETS; i++) {
        unix_socket_unref(all[i]);
    }
    CHECK_EQ(unix_socket_pair(UNIX_SOCKET_STREAM, &x, &y), 0);
    unix_socket_unref(x);
    unix_socket_unref(y);
    expect_nothing_left();
}

TEST(unix_socket, the_name_table_refuses_and_recovers) {
    clean();
    struct unix_socket *s[UNIX_MAX_NAMES + 1];
    for (int i = 0; i < UNIX_MAX_NAMES + 1; i++) {
        s[i] = unix_socket_alloc(UNIX_SOCKET_STREAM);
        REQUIRE(s[i] != NULL);
        char name[8] = {'/', 'n', (char)('0' + i % 10), (char)('a' + i / 10), 0};
        CHECK_EQ(unix_socket_bind(s[i], name, 4), i < UNIX_MAX_NAMES ? 0 : -1);
    }
    for (int i = 0; i < UNIX_MAX_NAMES + 1; i++) {
        unix_socket_unref(s[i]);
    }
    struct unix_socket *again = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(again != NULL);
    CHECK_EQ(unix_socket_bind(again, "/n0a", 4), 0);
    unix_socket_unref(again);
    expect_nothing_left();
}

TEST(unix_socket, a_zero_length_message_carrying_a_descriptor_is_a_message) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t one = a_pipe(1);
    CHECK_EQ(unix_socket_send(a, NULL, 0, &one, 1), 0);
    CHECK_EQ(unix_socket_pending(b), 1);
    file_descriptor_slot_t got[UNIX_MAX_FILE_DESCRIPTORS];
    int nfds = 0;
    uint8_t out[8];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, NULL), 0);
    CHECK_EQ(nfds, 1);
    file_descriptor_release(&got[0]);
    CHECK_EQ(unix_socket_send(a, NULL, 0, NULL, 0), 0);
    CHECK_EQ(unix_socket_pending(b), 0);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, too_many_descriptors_is_refused_before_anything_moves) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t many[UNIX_MAX_FILE_DESCRIPTORS + 1];
    for (int i = 0; i < UNIX_MAX_FILE_DESCRIPTORS + 1; i++) {
        many[i] = a_pipe(i);
    }
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"x", 1, many, UNIX_MAX_FILE_DESCRIPTORS + 1), -1);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    CHECK_EQ(unix_socket_pending(b), 0);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_type_that_does_not_exist_is_refused) {
    clean();
    CHECK(unix_socket_alloc(2) == NULL);
    CHECK(unix_socket_alloc(0) == NULL);
    CHECK(unix_socket_alloc(99) == NULL);
    struct unix_socket *x = NULL, *y = NULL;
    CHECK_EQ(unix_socket_pair(2, &x, &y), -1);
    CHECK(x == NULL && y == NULL);
    CHECK_EQ(unix_socket_in_use(), 0);
    expect_nothing_left();
}

TEST(unix_socket, a_socket_reports_its_own_type) {
    clean();
    struct unix_socket *s = unix_socket_alloc(UNIX_SOCKET_SEQPACKET);
    REQUIRE(s != NULL);
    CHECK_EQ(unix_socket_type(s), UNIX_SOCKET_SEQPACKET);
    unix_socket_unref(s);
    struct unix_socket *t = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(t != NULL);
    CHECK_EQ(unix_socket_type(t), UNIX_SOCKET_STREAM);
    unix_socket_unref(t);
    CHECK_EQ(unix_socket_type(NULL), -1);
    expect_nothing_left();
}

TEST(unix_socket, a_name_of_every_legal_length_and_none_that_is_not) {
    clean();
    struct unix_socket *s = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(s != NULL);
    CHECK_EQ(unix_socket_bind(s, "x", 0), -1);
    CHECK_EQ(unix_socket_bind(s, "x", -1), -1);
    CHECK_EQ(unix_socket_bind(s, NULL, 4), -1);
    char full[UNIX_PATH_MAX + 1];
    memset(full, 'n', sizeof(full));
    CHECK_EQ(unix_socket_bind(s, full, UNIX_PATH_MAX + 1), -1);
    CHECK_EQ(unix_socket_bind(s, full, UNIX_PATH_MAX), 0);
    struct unix_socket *c = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(c != NULL);
    CHECK_EQ(unix_socket_listen(s), 0);
    CHECK_EQ(unix_socket_connect(c, full, UNIX_PATH_MAX), 0);
    CHECK_EQ(unix_socket_connect(c, full, 0), -1);
    CHECK_EQ(unix_socket_connect(c, full, UNIX_PATH_MAX + 1), -1);
    struct unix_socket *conn = unix_socket_accept(s);
    REQUIRE(conn != NULL);
    unix_socket_unref(conn);
    unix_socket_unref(c);
    unix_socket_unref(s);
    expect_nothing_left();
}

TEST(unix_socket, a_message_that_does_not_fit_right_now_waits_whole) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &a, &b) == 0);
    static uint8_t buffer[UNIX_BUFFER_SIZE];
    memset(buffer, 'a', sizeof(buffer));
    CHECK_EQ(unix_socket_send(a, buffer, UNIX_BUFFER_SIZE - 8, NULL, 0), UNIX_BUFFER_SIZE - 8);
    CHECK_EQ(unix_socket_send(a, buffer, 16, NULL, 0), 0);
    uint8_t out[UNIX_BUFFER_SIZE];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), UNIX_BUFFER_SIZE - 8);
    CHECK_EQ(unix_socket_send(a, buffer, 16, NULL, 0), 16);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_stream_of_small_writes_does_not_exhaust_the_record_queue) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    for (int i = 0; i < UNIX_MAX_SEGS * 4; i++) {
        CHECK_EQ(unix_socket_send(a, (const uint8_t *)"s", 1, NULL, 0), 1);
    }
    uint8_t out[UNIX_MAX_SEGS * 4 + 8];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), UNIX_MAX_SEGS * 4);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, receive_clears_what_it_is_about_to_report) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"plain", 5, NULL, 0), 5);
    uint8_t out[16];
    file_descriptor_slot_t got[UNIX_MAX_FILE_DESCRIPTORS];
    int nfds = 7;
    int flags = 0xFF;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, &flags), 5);
    CHECK_EQ(nfds, 0);
    CHECK_EQ(flags, 0);
    nfds = 7;
    flags = 0xFF;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, &flags), 0);
    CHECK_EQ(nfds, 0);
    CHECK_EQ(flags, 0);
    unix_socket_unref(a);
    nfds = 7;
    flags = 0xFF;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, &flags), -1);
    CHECK_EQ(nfds, 0);
    CHECK_EQ(flags, 0);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, descriptors_are_delivered_once_even_when_the_record_survives) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t one = a_pipe(1);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"0123456789", 10, &one, 1), 10);
    uint8_t out[4];
    file_descriptor_slot_t got[UNIX_MAX_FILE_DESCRIPTORS];
    int nfds = 0;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, NULL), 4);
    CHECK_EQ(nfds, 1);
    file_descriptor_release(&got[0]);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    nfds = 0;
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), got, UNIX_MAX_FILE_DESCRIPTORS, &nfds, NULL), 4);
    CHECK_EQ(nfds, 0);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, a_truncated_message_does_not_take_the_next_one_with_it) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &a, &b) == 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"first-long", 10, NULL, 0), 10);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"second", 6, NULL, 0), 6);
    uint8_t out[8];
    int flags = 0;
    CHECK_EQ(unix_socket_receive(b, out, 4, NULL, 0, NULL, &flags), 4);
    CHECK_MEMEQ(out, "firs", 4);
    CHECK_EQ(flags & UNIX_RECEIVE_TRUNC, UNIX_RECEIVE_TRUNC);
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 6);
    CHECK_MEMEQ(out, "second", 6);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, shutdown_refuses_a_how_it_does_not_have) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    CHECK_EQ(unix_socket_shutdown(a, 3), -1);
    CHECK_EQ(unix_socket_shutdown(a, -1), -1);
    CHECK_EQ(unix_socket_shutdown(NULL, 1), -1);
    CHECK_EQ(unix_socket_shutdown(a, 2), 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"x", 1, NULL, 0), -1);
    uint8_t out[8];
    CHECK_EQ(unix_socket_receive(a, out, sizeof(out), NULL, 0, NULL, NULL), -1);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, writable_is_true_until_the_buffer_is_full) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    CHECK_EQ(unix_socket_writable(a), 1);
    static uint8_t big[UNIX_BUFFER_SIZE];
    memset(big, 'w', sizeof(big));
    CHECK_EQ(unix_socket_send(a, big, UNIX_BUFFER_SIZE, NULL, 0), UNIX_BUFFER_SIZE);
    CHECK_EQ(unix_socket_writable(a), 0);
    uint8_t out[64];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 64);
    CHECK_EQ(unix_socket_writable(a), 1);
    unix_socket_unref(b);
    CHECK_EQ(unix_socket_writable(a), 1);
    CHECK_EQ(unix_socket_send(a, big, 1, NULL, 0), -1);
    unix_socket_unref(a);
    expect_nothing_left();
}

TEST(unix_socket, a_record_queue_that_is_full_is_not_writable) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_SEQPACKET, &a, &b) == 0);
    for (int i = 0; i < UNIX_MAX_SEGS; i++) {
        CHECK_EQ(unix_socket_send(a, (const uint8_t *)"m", 1, NULL, 0), 1);
    }
    CHECK_EQ(unix_socket_writable(a), 0);
    uint8_t out[8];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 1);
    CHECK_EQ(unix_socket_writable(a), 1);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

TEST(unix_socket, hup_waits_for_the_queue_to_drain_and_rdhup_does_not) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    CHECK_EQ(unix_socket_hup(b), 0);
    CHECK_EQ(unix_socket_rdhup(b), 0);
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"request", 7, NULL, 0), 7);
    CHECK_EQ(unix_socket_shutdown(a, 1), 0);
    CHECK_EQ(unix_socket_rdhup(b), 1);
    CHECK_EQ(unix_socket_hup(b), 0);
    uint8_t out[16];
    CHECK_EQ(unix_socket_receive(b, out, sizeof(out), NULL, 0, NULL, NULL), 7);
    CHECK_EQ(unix_socket_hup(b), 1);
    CHECK_EQ(unix_socket_hup(a), 0);
    CHECK_EQ(unix_socket_rdhup(a), 0);
    CHECK_EQ(unix_socket_send(b, (const uint8_t *)"reply", 5, NULL, 0), 5);
    unix_socket_unref(a);
    CHECK_EQ(unix_socket_hup(b), 1);
    CHECK_EQ(unix_socket_rdhup(b), 1);
    unix_socket_unref(b);

    struct unix_socket *c = NULL, *d = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &c, &d) == 0);
    CHECK_EQ(unix_socket_send(c, (const uint8_t *)"last words", 10, NULL, 0), 10);
    unix_socket_unref(c);
    CHECK_EQ(unix_socket_hup(d), 0);
    CHECK_EQ(unix_socket_rdhup(d), 1);
    uint8_t tail[16];
    CHECK_EQ(unix_socket_receive(d, tail, sizeof(tail), NULL, 0, NULL, NULL), 10);
    CHECK_EQ(unix_socket_hup(d), 1);
    unix_socket_unref(d);
    expect_nothing_left();
}

TEST(unix_socket, the_epoll_accessors_refuse_a_null_socket) {
    clean();
    CHECK_EQ(unix_socket_writable(NULL), 0);
    CHECK_EQ(unix_socket_hup(NULL), 1);
    CHECK_EQ(unix_socket_rdhup(NULL), 1);
    expect_nothing_left();
}

/* M225: a slot that names nothing is not something SCM_RIGHTS can carry.
   sys_sendmsg checked only for NONE, so a descriptor another thread had
   claimed and not yet filled (RESERVED) went into the queue as one, and the
   receiver was handed a slot nobody would ever fill. Refused here, whole -
   nothing queued and no reference taken on the descriptors beside it. */
TEST(unix_socket, a_slot_that_names_nothing_is_not_sent) {
    clean();
    struct unix_socket *a = NULL, *b = NULL;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &a, &b) == 0);
    file_descriptor_slot_t two[2] = {a_pipe(1), a_pipe(2)};
    two[1].type = FILE_DESCRIPTOR_RESERVED;
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"x", 1, two, 2), -1);
    two[1].type = FILE_DESCRIPTOR_NONE;
    CHECK_EQ(unix_socket_send(a, (const uint8_t *)"x", 1, two, 2), -1);
    CHECK_EQ(unix_socket_queued_file_descriptors(), 0);
    CHECK_EQ(fake_objects_pipe_read_refs(), 0);
    CHECK_EQ(unix_socket_pending(b), 0);
    unix_socket_unref(a);
    unix_socket_unref(b);
    expect_nothing_left();
}

/* M226: the address a socket answers to. getsockname and getpeername on an
   AF_UNIX socket report it - libuv tells a pipe from TCP that way - and a
   connection's server end takes the listener's, so the client asked who it
   is talking to names the path it connected to, before and after accept. */
TEST(unix_socket, a_connection_names_the_path_it_was_made_to) {
    clean();
    char name[UNIX_PATH_MAX];
    struct unix_socket *pair_a, *pair_b;
    REQUIRE(unix_socket_pair(UNIX_SOCKET_STREAM, &pair_a, &pair_b) == 0);
    CHECK_EQ(unix_socket_name(pair_a, 0, name, sizeof(name)), 0);
    CHECK_EQ(unix_socket_name(pair_a, 1, name, sizeof(name)), 0);

    struct unix_socket *srv = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(srv != NULL);
    CHECK_EQ(unix_socket_name(srv, 1, name, sizeof(name)), -1);
    CHECK_EQ(unix_socket_bind(srv, "/tmp/named", 10), 0);
    CHECK_EQ(unix_socket_listen(srv), 0);
    CHECK_EQ(unix_socket_name(srv, 0, name, sizeof(name)), 10);
    CHECK(memcmp(name, "/tmp/named", 10) == 0);

    struct unix_socket *cli = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(cli != NULL);
    CHECK_EQ(unix_socket_connect(cli, "/tmp/named", 10), 0);
    memset(name, 0, sizeof(name));
    CHECK_EQ(unix_socket_name(cli, 1, name, sizeof(name)), 10);
    CHECK(memcmp(name, "/tmp/named", 10) == 0);
    CHECK_EQ(unix_socket_name(cli, 0, name, sizeof(name)), 0);

    struct unix_socket *conn = unix_socket_accept(srv);
    REQUIRE(conn != NULL);
    memset(name, 0, sizeof(name));
    CHECK_EQ(unix_socket_name(conn, 0, name, sizeof(name)), 10);
    CHECK(memcmp(name, "/tmp/named", 10) == 0);
    CHECK_EQ(unix_socket_name(conn, 0, name, 4), 4);

    const char abstract[] = {0, 'u', 'v'};
    struct unix_socket *hidden = unix_socket_alloc(UNIX_SOCKET_STREAM);
    REQUIRE(hidden != NULL);
    CHECK_EQ(unix_socket_bind(hidden, abstract, 3), 0);
    CHECK_EQ(unix_socket_name(hidden, 0, name, sizeof(name)), 3);
    CHECK(memcmp(name, abstract, 3) == 0);

    unix_socket_unref(hidden);
    unix_socket_unref(conn);
    unix_socket_unref(cli);
    unix_socket_unref(srv);
    unix_socket_unref(pair_a);
    unix_socket_unref(pair_b);
    expect_nothing_left();
}
