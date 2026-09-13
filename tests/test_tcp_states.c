#include "check.h"
#include "fakes/fakes.h"

#include "network/arp.h"
#include "network/ethernet.h"
#include "network/ip.h"
#include "network/tcp.h"

#include <stdint.h>
#include <string.h>

#define LOCAL_IP  0x0A00020Fu
#define PEER_IP   0x0A000202u
#define PEER_PORT 4321
#define OUR_PORT  8080

static const uint8_t peer_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

static void be16_put(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32_put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t be16_get(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32_get(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void tcp_fixture(void) {
    fake_net_reset();
    fake_socket_reset();
    klog_capture_reset();
    tcp_init();
    arp_learn(PEER_IP, peer_mac);
}

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t *seg, uint16_t seg_len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += 6;
    sum += seg_len;
    for (uint16_t i = 0; i + 1 < seg_len; i += 2) {
        sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
    }
    if (seg_len & 1) {
        sum += (uint32_t)(seg[seg_len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFF);
}

static void from_peer(uint16_t dst_port, uint16_t src_port, uint32_t seq,
                      uint32_t ack, uint8_t flags,
                      const uint8_t *payload, uint16_t payload_len) {
    uint8_t seg[20 + 128];
    memset(seg, 0, sizeof(seg));
    be16_put(seg + 0, src_port);
    be16_put(seg + 2, dst_port);
    be32_put(seg + 4, seq);
    be32_put(seg + 8, ack);
    seg[12] = 5 << 4;
    seg[13] = flags;
    be16_put(seg + 14, 4096);
    if (payload_len) {
        memcpy(seg + 20, payload, payload_len);
    }
    uint16_t total = (uint16_t)(20 + payload_len);
    be16_put(seg + 16, tcp_checksum(PEER_IP, LOCAL_IP, seg, total));
    tcp_handle_packet(PEER_IP, LOCAL_IP, seg, total);
}

static void from_peer_window(uint16_t dst_port, uint16_t src_port, uint32_t seq,
                             uint32_t ack, uint8_t flags, uint16_t window) {
    uint8_t seg[20];
    memset(seg, 0, sizeof(seg));
    be16_put(seg + 0, src_port);
    be16_put(seg + 2, dst_port);
    be32_put(seg + 4, seq);
    be32_put(seg + 8, ack);
    seg[12] = 5 << 4;
    seg[13] = flags;
    be16_put(seg + 14, window);
    be16_put(seg + 16, tcp_checksum(PEER_IP, LOCAL_IP, seg, 20));
    tcp_handle_packet(PEER_IP, LOCAL_IP, seg, 20);
}

typedef struct {
    uint16_t src_port, dst_port, window;
    uint32_t seq, ack;
    uint8_t flags;
    const uint8_t *payload;
    uint16_t payload_len;
} sent_t;

static int last_sent(sent_t *out) {
    int n = fake_net_tx_count();
    if (n == 0) {
        return 0;
    }
    uint32_t len = 0;
    const uint8_t *frame = fake_net_tx_frame(n - 1, &len);
    if (!frame || len < 14 + 20 + 20) {
        return 0;
    }
    const uint8_t *ip = frame + 14;
    uint16_t ihl = (uint16_t)((ip[0] & 0x0F) * 4);
    const uint8_t *tcp = ip + ihl;
    uint16_t total = be16_get(ip + 2);
    out->src_port = be16_get(tcp + 0);
    out->dst_port = be16_get(tcp + 2);
    out->seq = be32_get(tcp + 4);
    out->ack = be32_get(tcp + 8);
    out->flags = tcp[13];
    out->window = be16_get(tcp + 14);
    uint16_t data_off = (uint16_t)((tcp[12] >> 4) * 4);
    out->payload = tcp + data_off;
    out->payload_len = (uint16_t)(total - ihl - data_off);
    return 1;
}

static int sent_count_with(uint8_t flags) {
    int n = 0;
    for (int i = 0; i < fake_net_tx_count(); i++) {
        uint32_t len = 0;
        const uint8_t *frame = fake_net_tx_frame(i, &len);
        if (!frame || len < 14 + 40) { continue; }
        const uint8_t *ip = frame + 14;
        const uint8_t *tcp = ip + (ip[0] & 0x0F) * 4;
        if ((tcp[13] & flags) == flags) { n++; }
    }
    return n;
}

TEST(tcp_state, a_fresh_control_block_is_closed) {
    tcp_fixture();
    struct tcpcb *t = tcp_open();
    REQUIRE(t != NULL);
    CHECK_EQ(tcp_state(t), TCP_CLOSED);
    tcp_release(t);
}

TEST(tcp_state, bind_and_listen_reach_LISTEN) {
    tcp_fixture();
    struct tcpcb *t = tcp_open();
    REQUIRE(t != NULL);
    CHECK_EQ(tcp_bind(t, OUR_PORT), OUR_PORT);
    CHECK_EQ(tcp_listen(t), 0);
    CHECK_EQ(tcp_state(t), TCP_LISTEN);
    CHECK_EQ(tcp_accept_pending(t), 0);
    tcp_release(t);
}

TEST(tcp_state, a_segment_to_a_port_nobody_listens_on_is_answered_with_RST) {
    tcp_fixture();
    from_peer(9999, PEER_PORT, 1000, 0, F_SYN, NULL, 0);
    CHECK(sent_count_with(F_RST) >= 1);
}

static struct tcpcb *listening(void) {
    struct tcpcb *l = tcp_open();
    tcp_bind(l, OUR_PORT);
    tcp_listen(l);
    return l;
}

TEST(tcp_state, a_SYN_to_a_listener_is_answered_with_SYN_ACK) {
    tcp_fixture();
    struct tcpcb *l = listening();
    fake_net_reset();

    from_peer(OUR_PORT, PEER_PORT, 1000, 0, F_SYN, NULL, 0);

    sent_t s;
    REQUIRE(last_sent(&s));
    CHECK_EQ(s.flags & (F_SYN | F_ACK), F_SYN | F_ACK);
    CHECK_EQ(s.src_port, OUR_PORT);
    CHECK_EQ(s.dst_port, PEER_PORT);
    CHECK_EQ(s.ack, 1001);
    CHECK_NE(s.seq, 0);
    tcp_release(l);
}

TEST(tcp_state, the_third_segment_completes_the_handshake) {
    tcp_fixture();
    struct tcpcb *l = listening();
    fake_net_reset();
    from_peer(OUR_PORT, PEER_PORT, 1000, 0, F_SYN, NULL, 0);
    sent_t s;
    REQUIRE(last_sent(&s));
    uint32_t our_isn = s.seq;

    CHECK_EQ(tcp_accept_pending(l), 0);
    from_peer(OUR_PORT, PEER_PORT, 1001, our_isn + 1, F_ACK, NULL, 0);
    CHECK_EQ(tcp_accept_pending(l), 1);

    struct tcpcb *c = tcp_accept(l);
    REQUIRE(c != NULL);
    CHECK_EQ(tcp_state(c), TCP_ESTABLISHED);
    CHECK_EQ(tcp_remote_port(c), PEER_PORT);
    CHECK_EQ(tcp_remote_ip(c), PEER_IP);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, an_ACK_with_the_wrong_number_does_not_complete_the_handshake) {
    tcp_fixture();
    struct tcpcb *l = listening();
    fake_net_reset();
    from_peer(OUR_PORT, PEER_PORT, 1000, 0, F_SYN, NULL, 0);
    sent_t s;
    REQUIRE(last_sent(&s));

    const uint32_t wrong[] = {0, 1, s.seq, s.seq + 2, s.seq + 1000, 0xFFFFFFFFu};
    for (unsigned i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
        from_peer(OUR_PORT, PEER_PORT, 1001, wrong[i], F_ACK, NULL, 0);
        CHECK_EQ(tcp_accept_pending(l), 0);
    }
    from_peer(OUR_PORT, PEER_PORT, 1001, s.seq + 1, F_ACK, NULL, 0);
    CHECK_EQ(tcp_accept_pending(l), 1);
    struct tcpcb *c = tcp_accept(l);
    if (c) { tcp_release(c); }
    tcp_release(l);
}

static struct tcpcb *established(struct tcpcb **listener_out, uint32_t *peer_seq,
                                 uint32_t *our_seq) {
    struct tcpcb *l = listening();
    fake_net_reset();
    from_peer(OUR_PORT, PEER_PORT, 1000, 0, F_SYN, NULL, 0);
    sent_t s;
    if (!last_sent(&s)) { return NULL; }
    from_peer(OUR_PORT, PEER_PORT, 1001, s.seq + 1, F_ACK, NULL, 0);
    struct tcpcb *c = tcp_accept(l);
    if (listener_out) { *listener_out = l; }
    if (peer_seq) { *peer_seq = 1001; }
    if (our_seq) { *our_seq = s.seq + 1; }
    return c;
}

TEST(tcp_state, data_arriving_is_buffered_and_acknowledged) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    fake_net_reset();
    const uint8_t body[] = "hello";
    from_peer(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK | F_PSH, body, 5);

    CHECK_EQ(tcp_bytes_available(c), 5);
    uint8_t out[16] = {0};
    CHECK_EQ(tcp_recv(c, out, sizeof(out)), 5);
    CHECK_MEMEQ(out, body, 5);

    sent_t s;
    REQUIRE(last_sent(&s));
    CHECK(s.flags & F_ACK);
    CHECK_EQ(s.ack, pseq + 5);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_segment_out_of_order_is_not_delivered_as_if_it_were_in_order) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    const uint8_t future[] = "LATER";
    from_peer(OUR_PORT, PEER_PORT, pseq + 100, oseq, F_ACK, future, 5);
    CHECK_EQ(tcp_bytes_available(c), 0);

    const uint8_t now[] = "FIRST";
    from_peer(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, now, 5);
    uint8_t out[16] = {0};
    CHECK_EQ(tcp_recv(c, out, sizeof(out)), 5);
    CHECK_MEMEQ(out, "FIRST", 5);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_corrupt_segment_is_dropped_counted_and_reported) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    uint32_t before = tcp_checksum_failures();
    klog_capture_reset();

    uint8_t seg[20 + 5];
    memset(seg, 0, sizeof(seg));
    be16_put(seg + 0, PEER_PORT);
    be16_put(seg + 2, OUR_PORT);
    be32_put(seg + 4, pseq);
    be32_put(seg + 8, oseq);
    seg[12] = 5 << 4;
    seg[13] = F_ACK | F_PSH;
    be16_put(seg + 14, 4096);
    memcpy(seg + 20, "HELLO", 5);
    be16_put(seg + 16, tcp_checksum(PEER_IP, LOCAL_IP, seg, sizeof(seg)));
    seg[24] ^= 0x20;
    tcp_handle_packet(PEER_IP, LOCAL_IP, seg, sizeof(seg));

    CHECK_EQ(tcp_bytes_available(c), 0);
    CHECK_EQ(tcp_checksum_failures(), before + 1);
    CHECK(klog_capture_contains("[tcp] checksum: dropped a corrupt segment"));

    seg[24] ^= 0x20;
    tcp_handle_packet(PEER_IP, LOCAL_IP, seg, sizeof(seg));
    CHECK_EQ(tcp_bytes_available(c), 5);
    CHECK_EQ(tcp_checksum_failures(), before + 1);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_window_update_is_not_a_duplicate_ack) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);
    static uint8_t data[3000];
    memset(data, 'w', sizeof(data));
    CHECK_EQ(tcp_send(c, data, sizeof(data)), 3000);
    int rtx_before = tcp_debug_retransmits();
    from_peer_window(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, 1000);
    from_peer_window(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, 2000);
    from_peer_window(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, 3000);
    CHECK_EQ(tcp_debug_retransmits() - rtx_before, 0);
    rtx_before = tcp_debug_retransmits();
    from_peer_window(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, 3000);
    from_peer_window(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, 3000);
    from_peer_window(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, 3000);
    CHECK_EQ(tcp_debug_retransmits() - rtx_before, 1);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_duplicate_segment_is_acknowledged_but_not_delivered_twice) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    const uint8_t body[] = "dup";
    from_peer(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, body, 3);
    from_peer(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK, body, 3);
    CHECK_EQ(tcp_bytes_available(c), 3);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_peer_FIN_moves_us_to_CLOSE_WAIT_and_recv_reports_end_of_stream) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    from_peer(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK | F_FIN, NULL, 0);
    CHECK_EQ(tcp_state(c), TCP_CLOSE_WAIT);
    uint8_t out[8];
    CHECK_EQ(tcp_recv(c, out, sizeof(out)), -1);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_FIN_carried_with_data_is_taken_with_it) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    fake_net_reset();
    const uint8_t body[] = "last";
    from_peer(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK | F_PSH | F_FIN, body, 4);

    CHECK_EQ(tcp_state(c), TCP_CLOSE_WAIT);
    sent_t s;
    REQUIRE(last_sent(&s));
    CHECK(s.flags & F_ACK);
    CHECK_EQ(s.ack, pseq + 4 + 1);

    uint8_t out[8] = {0};
    CHECK_EQ(tcp_recv(c, out, sizeof(out)), 4);
    CHECK_MEMEQ(out, "last", 4);
    CHECK_EQ(tcp_recv(c, out, sizeof(out)), -1);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_FIN_behind_data_that_did_not_fit_is_not_taken) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    uint8_t chunk[100];
    memset(chunk, 'x', sizeof(chunk));
    uint32_t seq = pseq;
    while (tcp_bytes_available(c) + 100 <= TCP_RECV_BUF - 2) {
        from_peer(OUR_PORT, PEER_PORT, seq, oseq, F_ACK, chunk, 100);
        seq += 100;
    }
    int room = TCP_RECV_BUF - tcp_bytes_available(c);
    REQUIRE(room > 0 && room < 100);

    from_peer(OUR_PORT, PEER_PORT, seq, oseq, F_ACK | F_FIN, chunk, 100);
    CHECK_EQ(tcp_state(c), TCP_ESTABLISHED);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, closing_from_CLOSE_WAIT_reaches_LAST_ACK_then_CLOSED) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    from_peer(OUR_PORT, PEER_PORT, pseq, oseq, F_ACK | F_FIN, NULL, 0);
    REQUIRE(tcp_state(c) == TCP_CLOSE_WAIT);

    fake_net_reset();
    tcp_close(c);
    CHECK_EQ(tcp_state(c), TCP_LAST_ACK);
    sent_t s;
    REQUIRE(last_sent(&s));
    CHECK(s.flags & F_FIN);

    from_peer(OUR_PORT, PEER_PORT, pseq + 1, s.seq + 1, F_ACK, NULL, 0);
    CHECK_EQ(tcp_state(c), TCP_CLOSED);
    tcp_release(l);
}

TEST(tcp_state, an_active_close_goes_FIN_WAIT_1_then_FIN_WAIT_2_then_TIME_WAIT) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    fake_net_reset();
    tcp_close(c);
    CHECK_EQ(tcp_state(c), TCP_FIN_WAIT_1);
    sent_t s;
    REQUIRE(last_sent(&s));
    CHECK(s.flags & F_FIN);
    uint32_t our_fin_seq = s.seq;

    from_peer(OUR_PORT, PEER_PORT, pseq, our_fin_seq + 1, F_ACK, NULL, 0);
    CHECK_EQ(tcp_state(c), TCP_FIN_WAIT_2);

    from_peer(OUR_PORT, PEER_PORT, pseq, our_fin_seq + 1, F_ACK | F_FIN, NULL, 0);
    CHECK_EQ(tcp_state(c), TCP_TIME_WAIT);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_simultaneous_close_goes_through_CLOSING) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    fake_net_reset();
    tcp_close(c);
    REQUIRE(tcp_state(c) == TCP_FIN_WAIT_1);
    sent_t s;
    REQUIRE(last_sent(&s));

    from_peer(OUR_PORT, PEER_PORT, pseq, s.seq, F_ACK | F_FIN, NULL, 0);
    CHECK_EQ(tcp_state(c), TCP_CLOSING);

    from_peer(OUR_PORT, PEER_PORT, pseq + 1, s.seq + 1, F_ACK, NULL, 0);
    CHECK_EQ(tcp_state(c), TCP_TIME_WAIT);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, TIME_WAIT_expires_on_the_clock_rather_than_on_a_segment) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    tcp_close(c);
    sent_t s;
    REQUIRE(last_sent(&s));
    from_peer(OUR_PORT, PEER_PORT, pseq, s.seq + 1, F_ACK, NULL, 0);
    from_peer(OUR_PORT, PEER_PORT, pseq, s.seq + 1, F_ACK | F_FIN, NULL, 0);
    REQUIRE(tcp_state(c) == TCP_TIME_WAIT);

    for (int i = 0; i < 2000 && tcp_state(c) == TCP_TIME_WAIT; i++) {
        tcp_tick();
    }
    CHECK_EQ(tcp_state(c), TCP_CLOSED);
    tcp_release(l);
}

TEST(tcp_state, a_RST_tears_down_an_established_connection) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    from_peer(OUR_PORT, PEER_PORT, pseq, oseq, F_RST, NULL, 0);
    CHECK_NE(tcp_state(c), TCP_ESTABLISHED);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_RST_with_a_sequence_outside_the_window_is_ignored) {
    tcp_fixture();
    struct tcpcb *l = NULL;
    uint32_t pseq = 0, oseq = 0;
    struct tcpcb *c = established(&l, &pseq, &oseq);
    REQUIRE(c != NULL);

    from_peer(OUR_PORT, PEER_PORT, pseq + 100000, oseq, F_RST, NULL, 0);
    CHECK_EQ(tcp_state(c), TCP_ESTABLISHED);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, a_RST_in_SYN_RECEIVED_returns_the_connection_rather_than_leaking_it) {
    tcp_fixture();
    struct tcpcb *l = listening();
    for (int i = 0; i < TCP_MAX_TCBS * 4; i++) {
        fake_net_reset();
        from_peer(OUR_PORT, (uint16_t)(5000 + i), 1000, 0, F_SYN, NULL, 0);
        sent_t s;
        if (!last_sent(&s)) {
            CHECK(0 && "a SYN went unanswered - the tcb table has leaked");
            break;
        }
        CHECK(s.flags & F_SYN);
        from_peer(OUR_PORT, (uint16_t)(5000 + i), 1001, s.seq + 1, F_RST, NULL, 0);
    }
    tcp_release(l);
}

TEST(tcp_state, sequence_numbers_wrap_through_zero_correctly) {
    tcp_fixture();
    struct tcpcb *l = listening();
    fake_net_reset();

    const uint32_t near_top = 0xFFFFFFF0u;
    from_peer(OUR_PORT, PEER_PORT, near_top, 0, F_SYN, NULL, 0);
    sent_t s;
    REQUIRE(last_sent(&s));
    CHECK_EQ(s.ack, near_top + 1);

    from_peer(OUR_PORT, PEER_PORT, near_top + 1, s.seq + 1, F_ACK, NULL, 0);
    struct tcpcb *c = tcp_accept(l);
    REQUIRE(c != NULL);

    uint8_t body[32];
    for (int i = 0; i < 32; i++) { body[i] = (uint8_t)i; }
    from_peer(OUR_PORT, PEER_PORT, near_top + 1, s.seq + 1, F_ACK, body, 32);
    CHECK_EQ(tcp_bytes_available(c), 32);
    uint8_t out[32] = {0};
    CHECK_EQ(tcp_recv(c, out, sizeof(out)), 32);
    CHECK_MEMEQ(out, body, 32);
    tcp_release(c);
    tcp_release(l);
}

TEST(tcp_state, every_flag_combination_in_every_state_leaves_a_legal_state) {
    tcp_fixture();
    for (unsigned flags = 0; flags < 64; flags++) {
        tcp_fixture();
        struct tcpcb *l = NULL;
        uint32_t pseq = 0, oseq = 0;
        struct tcpcb *c = established(&l, &pseq, &oseq);
        REQUIRE(c != NULL);

        CHECK_NO_PANIC(from_peer(OUR_PORT, PEER_PORT, pseq, oseq, (uint8_t)flags, NULL, 0));
        tcp_state_t st = tcp_state(c);
        CHECK(st >= TCP_CLOSED && st <= TCP_TIME_WAIT);

        CHECK_NO_PANIC(tcp_tick());
        tcp_release(c);
        tcp_release(l);
    }
}

TEST(tcp_state, the_control_block_table_is_returned_across_many_connections) {
    tcp_fixture();
    struct tcpcb *l = listening();
    for (int i = 0; i < TCP_MAX_TCBS * 4; i++) {
        fake_net_reset();
        uint16_t port = (uint16_t)(6000 + i);
        from_peer(OUR_PORT, port, 1000, 0, F_SYN, NULL, 0);
        sent_t s;
        REQUIRE(last_sent(&s));
        from_peer(OUR_PORT, port, 1001, s.seq + 1, F_ACK, NULL, 0);
        struct tcpcb *c = tcp_accept(l);
        REQUIRE(c != NULL);
        CHECK_EQ(tcp_state(c), TCP_ESTABLISHED);
        tcp_abort(c);
    }
    tcp_release(l);
}
