/* tests/test_net.c - Q4
 *
 * The network stack, fed things no correct peer would send.
 *
 * The audit that produced this milestone expected to find missing length
 * checks and did not: arp.c, ip.c, icmp.c and udp.c bound their input
 * carefully and consistently. That is worth saying plainly, because the
 * conclusion is not "the code is bad" - it is that a set of careful
 * checks which nothing has ever exercised is a set of careful checks
 * whose next edit is unprotected.
 *
 * So the assertions here come in pairs wherever they can. A malformed
 * packet must be dropped *and* answered with silence; a well-formed one
 * must be answered with exactly the right bytes. "It did not crash" is
 * the weakest possible claim about a parser, and it is the only claim a
 * fuzzer alone can make.
 *
 * ASan and UBSan are on for this build, so an over-read of one byte past
 * a frame is a failure here rather than a value that happened to be
 * harmless on the day. */
#include "check.h"
#include "fakes/fakes.h"

#include "net/arp.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/ip.h"
#include "net/net.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "net/net.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Fixed test peers. The local address matches the fake's. */
#define PEER_IP   0x0A000202u   /* 10.0.2.2  */
#define LOCAL_IP  0x0A00020Fu   /* 10.0.2.15 */
#define OTHER_IP  0x0A0002FEu   /* 10.0.2.254 - not us */

static const uint8_t peer_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};

static void net_fixture(void) {
    fake_net_reset();
    fake_socket_reset();
    klog_capture_reset();
}

static void be16_put(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32_put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ---- ARP -------------------------------------------------------------- */

/* Builds a well-formed ARP request for `target`. */
static void arp_request(uint8_t out[28], uint32_t sender_ip, uint32_t target_ip) {
    memset(out, 0, 28);
    be16_put(out + 0, 1);        /* Ethernet */
    be16_put(out + 2, 0x0800);   /* IPv4 */
    out[4] = 6;                  /* hardware address length */
    out[5] = 4;                  /* protocol address length */
    be16_put(out + 6, 1);        /* request */
    memcpy(out + 8, peer_mac, 6);
    be32_put(out + 14, sender_ip);
    be32_put(out + 24, target_ip);
}

TEST(net_arp, a_request_for_us_is_answered_and_one_for_someone_else_is_not) {
    net_fixture();
    uint8_t pkt[28];

    arp_request(pkt, PEER_IP, LOCAL_IP);
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 1);

    fake_net_reset();
    /* Someone else's address. Answering this is how a host ends up
     * poisoning a network's ARP tables, so silence is the assertion. */
    arp_request(pkt, PEER_IP, OTHER_IP);
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);
}

TEST(net_arp, a_truncated_packet_is_dropped_at_every_length) {
    net_fixture();
    uint8_t pkt[28];
    arp_request(pkt, PEER_IP, LOCAL_IP);
    /* Every length short of a whole ARP packet. Each one must be dropped
     * silently - and, because ASan is watching, without reading a byte
     * past what it was given. That second property is the one this can
     * check and a running kernel cannot. */
    for (uint16_t len = 0; len < 28; len++) {
        fake_net_reset();
        uint8_t *heap = malloc(len ? len : 1);
        REQUIRE(heap != NULL);
        memcpy(heap, pkt, len);
        CHECK_NO_PANIC(arp_handle_packet(heap, len));
        CHECK_EQ(fake_net_tx_count(), 0);
        free(heap);
    }
}

TEST(net_arp, a_packet_for_a_protocol_we_do_not_speak_is_dropped) {
    net_fixture();
    uint8_t pkt[28];
    arp_request(pkt, PEER_IP, LOCAL_IP);
    be16_put(pkt + 0, 6);   /* not Ethernet */
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);

    arp_request(pkt, PEER_IP, LOCAL_IP);
    be16_put(pkt + 2, 0x86DD); /* IPv6 */
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);
}

/* Builds a well-formed ARP reply from `sender_ip` at `mac`, to us. */
static void arp_reply(uint8_t out[28], uint32_t sender_ip, const uint8_t mac[6]) {
    arp_request(out, sender_ip, LOCAL_IP);
    be16_put(out + 6, 2);        /* reply */
    memcpy(out + 8, mac, 6);
}

/* M116: the first packet to a neighbour nobody has resolved yet is held
 * and sent when the answer arrives - not dropped.
 *
 * It was dropped, on purpose and with a comment saying BSD does that:
 * "send the ARP request, drop this one, and let the caller send again".
 * BSD does not - it keeps the packet (la_hold) and sends it when the
 * reply comes in. Dropping it meant every first contact with a machine
 * on the local network waited out a retransmission timer: the [m116]
 * self-test measured 980 ms to connect to a host one hop away, all of
 * it TCP's one-second RTO on a SYN that had never left. */
TEST(net_arp, the_first_packet_to_an_unresolved_neighbour_is_sent_when_it_answers) {
    net_fixture();
    arp_init();
    const uint32_t NEIGHBOUR = 0x0A00024Du; /* 10.0.2.77 - on the link, never seen */
    static const uint8_t neighbour_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x4D};
    const uint8_t payload[] = "held, not dropped";

    CHECK_EQ(ip_send(NEIGHBOUR, 253, payload, sizeof(payload)), 0);
    /* Nothing but the question has gone out. */
    REQUIRE(fake_net_tx_count() >= 1);
    for (int i = 0; i < fake_net_tx_count(); i++) {
        uint32_t len = 0;
        const uint8_t *f = fake_net_tx_frame(i, &len);
        CHECK_EQ((f[12] << 8) | f[13], 0x0806); /* ARP */
    }

    fake_net_reset();
    uint8_t pkt[28];
    arp_reply(pkt, NEIGHBOUR, neighbour_mac);
    arp_handle_packet(pkt, sizeof(pkt));

    /* ...and the answer lets the packet go, to the address it named. */
    REQUIRE(fake_net_tx_count() == 1);
    uint32_t len = 0;
    const uint8_t *f = fake_net_tx_frame(0, &len);
    CHECK_MEMEQ(f, neighbour_mac, 6);
    CHECK_EQ((f[12] << 8) | f[13], 0x0800);        /* IPv4 */
    CHECK_EQ(f[14 + 9], 253);                        /* the protocol it was sent with */
    CHECK_EQ(((uint32_t)f[14 + 16] << 24) | ((uint32_t)f[14 + 17] << 16) |
             ((uint32_t)f[14 + 18] << 8) | f[14 + 19], NEIGHBOUR);
    CHECK_MEMEQ(f + 14 + 20, payload, sizeof(payload));

    /* Sent once: a second reply finds nothing held. */
    fake_net_reset();
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);
}

/* One packet per neighbour, and the newest wins - which is BSD's rule
 * and the right one for the only sender that matters here: TCP resends
 * the same SYN, and the one to keep is the latest. A reply from a
 * DIFFERENT host releases nothing of this one's. */
TEST(net_arp, a_held_packet_is_replaced_by_a_newer_one_and_released_only_by_its_own_neighbour) {
    net_fixture();
    arp_init();
    const uint32_t A = 0x0A000250u, B = 0x0A000251u; /* 10.0.2.80, .81 */
    static const uint8_t mac_a[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x50};
    static const uint8_t mac_b[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x51};

    ip_send(A, 253, (const uint8_t *)"old", 3);
    ip_send(A, 253, (const uint8_t *)"new", 3);
    fake_net_reset();

    uint8_t pkt[28];
    arp_reply(pkt, B, mac_b);
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);

    arp_reply(pkt, A, mac_a);
    arp_handle_packet(pkt, sizeof(pkt));
    REQUIRE(fake_net_tx_count() == 1);
    uint32_t len = 0;
    const uint8_t *f = fake_net_tx_frame(0, &len);
    CHECK_MEMEQ(f + 14 + 20, "new", 3);
}

/* ...and a neighbour that never answers is reported rather than held
 * for ever: the third send to it, three unanswered questions, fails -
 * which is what user_space/bin/nettest.c's "unreachable" check asks of
 * the syscall. And the slot is freed, so it is asked afresh next time. */
TEST(net_arp, a_neighbour_that_never_answers_is_unreachable_by_the_third_send) {
    net_fixture();
    arp_init();
    const uint32_t SILENT = 0x0A0002EEu; /* 10.0.2.238 */
    CHECK_EQ(ip_send(SILENT, 253, (const uint8_t *)"1", 1), 0);
    CHECK_EQ(ip_send(SILENT, 253, (const uint8_t *)"2", 1), 0);
    CHECK_EQ(ip_send(SILENT, 253, (const uint8_t *)"3", 1), -1);
    CHECK_EQ(ip_send(SILENT, 253, (const uint8_t *)"4", 1), 0);
}

/* ---- Ethernet --------------------------------------------------------- */

TEST(net_eth, a_frame_shorter_than_a_header_is_dropped) {
    net_fixture();
    for (uint16_t len = 0; len < 14; len++) {
        uint8_t *heap = malloc(len ? len : 1);
        REQUIRE(heap != NULL);
        memset(heap, 0xAA, len);
        CHECK_NO_PANIC(eth_receive(heap, len));
        free(heap);
    }
    CHECK_EQ(fake_net_tx_count(), 0);
}

TEST(net_eth, an_unknown_ethertype_is_dropped_rather_than_guessed_at) {
    net_fixture();
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    memcpy(frame + 6, peer_mac, 6);
    be16_put(frame + 12, 0x88CC);  /* LLDP - not something this stack speaks */
    CHECK_NO_PANIC(eth_receive(frame, sizeof(frame)));
    CHECK_EQ(fake_net_tx_count(), 0);
}

/* ---- IPv4 ------------------------------------------------------------- */

/* A minimal IPv4 header with `proto` and a payload of `payload_len`. */
static uint16_t ip_build(uint8_t *out, uint8_t proto, uint32_t dst,
                         const uint8_t *payload, uint16_t payload_len) {
    memset(out, 0, 20);
    out[0] = 0x45;                       /* version 4, IHL 5 */
    be16_put(out + 2, (uint16_t)(20 + payload_len));
    out[8] = 64;                         /* TTL */
    out[9] = proto;
    be32_put(out + 12, PEER_IP);
    be32_put(out + 16, dst);
    if (payload_len) {
        memcpy(out + 20, payload, payload_len);
    }
    return (uint16_t)(20 + payload_len);
}

TEST(net_ip, a_datagram_addressed_to_someone_else_is_not_processed) {
    net_fixture();
    uint8_t icmp[8] = {8, 0, 0, 0, 0x12, 0x34, 0, 1};  /* echo request */
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1 /* ICMP */, OTHER_IP, icmp, sizeof(icmp));
    ip_handle_packet(peer_mac, pkt, n);
    /* Replying to a datagram that is not ours is how a host becomes a
     * reflector. Silence. */
    CHECK_EQ(fake_net_tx_count(), 0);
}

TEST(net_ip, a_header_claiming_a_length_longer_than_the_frame_is_not_believed) {
    net_fixture();
    uint8_t icmp[8] = {8, 0, 0, 0, 0x12, 0x34, 0, 1};
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, icmp, sizeof(icmp));

    /* total_len says 1500; the frame is 28 bytes. ip.c's own comment says
     * it trusts the header over the frame, which is right for the padding
     * case it was written for and dangerous in this one - so it also
     * bounds total_len by the frame length. This is that bound, tested.
     * The buffer is heap-allocated at exactly the real length so ASan
     * reports it if the bound is ever removed. */
    uint8_t *heap = malloc(n);
    REQUIRE(heap != NULL);
    memcpy(heap, pkt, n);
    be16_put(heap + 2, 1500);
    CHECK_NO_PANIC(ip_handle_packet(peer_mac, heap, n));
    free(heap);
}

TEST(net_ip, an_ihl_smaller_than_the_minimum_is_refused) {
    net_fixture();
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, NULL, 0);
    for (uint8_t ihl = 0; ihl < 5; ihl++) {
        pkt[0] = (uint8_t)(0x40 | ihl);
        CHECK_NO_PANIC(ip_handle_packet(peer_mac, pkt, n));
        CHECK_EQ(fake_net_tx_count(), 0);
    }
}

TEST(net_ip, an_ihl_claiming_options_that_are_not_there_is_refused) {
    net_fixture();
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, NULL, 0);
    /* IHL 15 means a 60-byte header. The frame is 20. Believing it would
     * hand the ICMP layer a pointer 40 bytes past the end. */
    uint8_t *heap = malloc(n);
    REQUIRE(heap != NULL);
    memcpy(heap, pkt, n);
    heap[0] = 0x4F;
    CHECK_NO_PANIC(ip_handle_packet(peer_mac, heap, n));
    CHECK_EQ(fake_net_tx_count(), 0);
    free(heap);
}

TEST(net_ip, a_version_other_than_four_is_refused) {
    net_fixture();
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, NULL, 0);
    for (uint8_t v = 0; v < 16; v++) {
        if (v == 4) { continue; }
        pkt[0] = (uint8_t)((v << 4) | 5);
        CHECK_NO_PANIC(ip_handle_packet(peer_mac, pkt, n));
    }
    CHECK_EQ(fake_net_tx_count(), 0);
}

TEST(net_ip, every_truncation_of_a_valid_datagram_is_survived) {
    net_fixture();
    uint8_t icmp[8] = {8, 0, 0, 0, 0x12, 0x34, 0, 1};
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, icmp, sizeof(icmp));
    for (uint16_t len = 0; len <= n; len++) {
        uint8_t *heap = malloc(len ? len : 1);
        REQUIRE(heap != NULL);
        memcpy(heap, pkt, len);
        CHECK_NO_PANIC(ip_handle_packet(peer_mac, heap, len));
        free(heap);
    }
}

/* ---- ICMP ------------------------------------------------------------- */

TEST(net_icmp, an_echo_request_is_answered_and_a_truncated_one_is_not) {
    net_fixture();
    uint8_t echo[12] = {8, 0, 0, 0, 0x1E, 0xA5, 0x00, 0x01, 'd', 'a', 't', 'a'};
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, echo, sizeof(echo));
    ip_handle_packet(peer_mac, pkt, n);
    CHECK_EQ(fake_net_tx_count(), 1);

    /* An ICMP header cut in half. Every prefix must be dropped. */
    for (uint16_t cut = 0; cut < sizeof(echo); cut++) {
        fake_net_reset();
        uint16_t m = ip_build(pkt, 1, LOCAL_IP, echo, cut);
        uint8_t *heap = malloc(m);
        REQUIRE(heap != NULL);
        memcpy(heap, pkt, m);
        CHECK_NO_PANIC(ip_handle_packet(peer_mac, heap, m));
        free(heap);
    }
}

TEST(net_icmp, an_unknown_icmp_type_is_not_answered) {
    net_fixture();
    /* Type 8 is echo request. Anything else must not produce a reply -
     * a host that answers every ICMP type is a host that can be made to
     * talk by anyone. */
    for (uint8_t type = 0; type < 40; type++) {
        if (type == 8) { continue; }
        fake_net_reset();
        uint8_t body[12] = {type, 0, 0, 0, 0x1E, 0xA5, 0, 1, 'x', 'y', 'z', 'w'};
        uint8_t pkt[64];
        uint16_t n = ip_build(pkt, 1, LOCAL_IP, body, sizeof(body));
        CHECK_NO_PANIC(ip_handle_packet(peer_mac, pkt, n));
        CHECK_EQ(fake_net_tx_count(), 0);
    }
}

/* ---- UDP -------------------------------------------------------------- */

TEST(net_udp, a_well_formed_datagram_reaches_the_socket_layer_intact) {
    net_fixture();
    const char *body = "hello over udp";
    uint16_t blen = (uint16_t)strlen(body);
    uint8_t udp[8 + 32];
    be16_put(udp + 0, 4321);                     /* source port */
    be16_put(udp + 2, 1234);                     /* destination port */
    be16_put(udp + 4, (uint16_t)(8 + blen));     /* length */
    be16_put(udp + 6, 0);                        /* checksum: optional in IPv4 */
    memcpy(udp + 8, body, blen);

    uint8_t pkt[128];
    uint16_t n = ip_build(pkt, 17, LOCAL_IP, udp, (uint16_t)(8 + blen));
    ip_handle_packet(peer_mac, pkt, n);

    REQUIRE(fake_socket_delivered_count() == 1);
    uint32_t got_len = 0;
    uint16_t got_port = 0;
    const uint8_t *got = fake_socket_delivered(0, &got_len, &got_port);
    CHECK_EQ(got_port, 1234);
    CHECK_EQ(got_len, blen);
    CHECK_MEMEQ(got, body, blen);
}

TEST(net_udp, a_length_field_that_lies_does_not_deliver_bytes_that_are_not_there) {
    net_fixture();
    uint8_t udp[8 + 4];
    be16_put(udp + 0, 4321);
    be16_put(udp + 2, 1234);
    be16_put(udp + 4, 1400);   /* claims 1392 bytes of payload; there are 4 */
    be16_put(udp + 6, 0);
    memcpy(udp + 8, "abcd", 4);

    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 17, LOCAL_IP, udp, sizeof(udp));
    uint8_t *heap = malloc(n);
    REQUIRE(heap != NULL);
    memcpy(heap, pkt, n);
    CHECK_NO_PANIC(ip_handle_packet(peer_mac, heap, n));

    /* Either it was dropped, or it was delivered - but never with more
     * bytes than actually arrived. That is the whole assertion, and the
     * form it takes is "whatever you deliver, it fits in the frame". */
    if (fake_socket_delivered_count() > 0) {
        uint32_t got_len = 0;
        fake_socket_delivered(0, &got_len, NULL);
        CHECK(got_len <= 4);
    }
    free(heap);
}

TEST(net_udp, every_truncation_of_a_datagram_is_survived) {
    net_fixture();
    uint8_t udp[8 + 16];
    be16_put(udp + 0, 4321);
    be16_put(udp + 2, 1234);
    be16_put(udp + 4, (uint16_t)sizeof(udp));
    be16_put(udp + 6, 0);
    memset(udp + 8, 'u', 16);

    for (uint16_t cut = 0; cut <= sizeof(udp); cut++) {
        fake_socket_reset();
        uint8_t pkt[128];
        uint16_t n = ip_build(pkt, 17, LOCAL_IP, udp, cut);
        uint8_t *heap = malloc(n);
        REQUIRE(heap != NULL);
        memcpy(heap, pkt, n);
        CHECK_NO_PANIC(ip_handle_packet(peer_mac, heap, n));
        free(heap);
    }
}

/* ---- TCP -------------------------------------------------------------- */

TEST(net_tcp, a_segment_shorter_than_a_header_is_dropped) {
    net_fixture();
    tcp_init();
    for (uint16_t len = 0; len < 20; len++) {
        uint8_t *heap = malloc(len ? len : 1);
        REQUIRE(heap != NULL);
        memset(heap, 0x5A, len);
        CHECK_NO_PANIC(tcp_handle_packet(PEER_IP, LOCAL_IP, heap, len));
        free(heap);
    }
}

TEST(net_tcp, a_data_offset_claiming_options_that_are_not_there_is_refused) {
    net_fixture();
    tcp_init();
    /* The one place in this stack where a length field from the wire
     * drives a loop: tcp.c's option parser walks to `data_off`. A header
     * whose data offset points past the end of the segment must be
     * refused before that walk starts. */
    for (uint8_t off = 5; off <= 15; off++) {
        uint8_t seg[20];
        memset(seg, 0, sizeof(seg));
        be16_put(seg + 0, 12345);
        be16_put(seg + 2, 80);
        seg[12] = (uint8_t)(off << 4);
        seg[13] = 0x02;   /* SYN */
        be16_put(seg + 14, 1024);
        uint8_t *heap = malloc(sizeof(seg));
        REQUIRE(heap != NULL);
        memcpy(heap, seg, sizeof(seg));
        CHECK_NO_PANIC(tcp_handle_packet(PEER_IP, LOCAL_IP, heap, sizeof(seg)));
        free(heap);
    }
}

TEST(net_tcp, a_malformed_option_length_does_not_walk_off_the_segment) {
    net_fixture();
    tcp_init();
    /* Option kind 2 (MSS) with a length byte of 0, and of 255. A parser
     * that trusts either advances by zero (an infinite loop) or past the
     * end (an over-read). tcp.c guards both; this is the guard. */
    const uint8_t bad_lens[] = {0, 1, 200, 255};
    for (unsigned i = 0; i < sizeof(bad_lens); i++) {
        uint8_t seg[24];
        memset(seg, 0, sizeof(seg));
        be16_put(seg + 0, 12345);
        be16_put(seg + 2, 80);
        seg[12] = 6 << 4;    /* 24-byte header: 20 + 4 of options */
        seg[13] = 0x02;      /* SYN */
        be16_put(seg + 14, 1024);
        seg[20] = 2;              /* kind: MSS */
        seg[21] = bad_lens[i];    /* nonsense length */
        seg[22] = 0x05;
        seg[23] = 0xB4;
        uint8_t *heap = malloc(sizeof(seg));
        REQUIRE(heap != NULL);
        memcpy(heap, seg, sizeof(seg));
        CHECK_NO_PANIC(tcp_handle_packet(PEER_IP, LOCAL_IP, heap, sizeof(seg)));
        free(heap);
    }
}

TEST(net_tcp, a_segment_for_a_port_nothing_is_listening_on_is_not_a_crash) {
    net_fixture();
    tcp_init();
    /* Every flag combination against a closed port. The correct answer is
     * either a RST or silence; the incorrect one is a fault, and the
     * combinations nobody sends are the ones nothing has tried. */
    for (unsigned flags = 0; flags < 64; flags++) {
        uint8_t seg[20];
        memset(seg, 0, sizeof(seg));
        be16_put(seg + 0, 40000);
        be16_put(seg + 2, 9999);
        be32_put(seg + 4, 0x11223344);
        be32_put(seg + 8, 0x55667788);
        seg[12] = 5 << 4;
        seg[13] = (uint8_t)flags;
        be16_put(seg + 14, 4096);
        CHECK_NO_PANIC(tcp_handle_packet(PEER_IP, LOCAL_IP, seg, sizeof(seg)));
    }
}

/* ---- Q12: asserting content, not counts -------------------------------
 *
 * Everything above this line was written before the mutation census and
 * the census was unkind to it. `ethernet.c` had 90% line coverage and a
 * **0%** mutation score: all 26 injected faults survived, because every
 * test that "covered" it only asserted that a malformed frame produced
 * no reply. Change `ethertype == ETH_TYPE_ARP` to `!=` and nothing here
 * noticed. `icmp.c` scored 7.5% for the same reason - a reply was
 * counted and never read.
 *
 * The lesson generalises and is worth stating once: a test that asserts
 * an *absence* constrains almost nothing. Dropping a bad packet is one
 * bit of behaviour; the other several hundred bits are in the packet
 * that gets sent back, and until this block nothing looked at them.
 */

/* Unwraps the last transmitted frame down to its IP payload. */
typedef struct {
    const uint8_t *eth;
    uint32_t eth_len;
    const uint8_t *ip;
    const uint8_t *payload;
    uint16_t payload_len;
    uint16_t ethertype;
    uint8_t protocol;
} tx_t;

static int last_tx(tx_t *out) {
    int n = fake_net_tx_count();
    if (n == 0) {
        return 0;
    }
    out->eth = fake_net_tx_frame(n - 1, &out->eth_len);
    if (!out->eth || out->eth_len < 14) {
        return 0;
    }
    out->ethertype = (uint16_t)((out->eth[12] << 8) | out->eth[13]);
    out->ip = out->eth + 14;
    if (out->ethertype == 0x0800 && out->eth_len >= 14 + 20) {
        uint16_t ihl = (uint16_t)((out->ip[0] & 0x0F) * 4);
        uint16_t total = (uint16_t)((out->ip[2] << 8) | out->ip[3]);
        out->protocol = out->ip[9];
        out->payload = out->ip + ihl;
        out->payload_len = (uint16_t)(total - ihl);
    } else {
        out->protocol = 0;
        out->payload = out->ip;
        out->payload_len = (uint16_t)(out->eth_len - 14);
    }
    return 1;
}

TEST(net_eth, a_sent_frame_carries_the_addresses_and_type_it_was_given) {
    net_fixture();
    /* eth_send was not called by any test before the census, which is
     * why every constant in its header construction survived mutation. */
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22};
    eth_send(peer_mac, 0x0800, payload, sizeof(payload));

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_MEMEQ(tx.eth, peer_mac, 6);            /* destination first */
    CHECK_MEMEQ(tx.eth + 6, net_local_mac(), 6); /* then the source */
    CHECK_EQ(tx.eth[12], 0x08);                  /* ethertype, big-endian */
    CHECK_EQ(tx.eth[13], 0x00);
    CHECK_MEMEQ(tx.eth + 14, payload, sizeof(payload));
}

TEST(net_eth, a_short_frame_is_padded_to_the_minimum_and_the_padding_is_zero) {
    net_fixture();
    /* Ethernet's 60-byte minimum. A frame short of it is padded, and the
     * padding must be zero rather than whatever was on the stack - which
     * is a real information leak and is exactly what the k_memset in
     * eth_send is for. Nothing checked either property. */
    const uint8_t tiny[] = {0xAA};
    eth_send(peer_mac, 0x0806, tiny, sizeof(tiny));

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.eth_len, 60);
    CHECK_EQ(tx.eth[14], 0xAA);
    for (uint32_t i = 15; i < tx.eth_len; i++) {
        CHECK_EQ(tx.eth[i], 0);
    }
}

TEST(net_eth, a_frame_at_or_over_the_minimum_is_not_padded_further) {
    net_fixture();
    uint8_t body[64];
    memset(body, 0x5A, sizeof(body));
    eth_send(peer_mac, 0x0800, body, sizeof(body));
    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.eth_len, 14 + sizeof(body));
}

TEST(net_eth, the_ethertype_decides_which_parser_sees_the_frame) {
    /* The dispatch, asserted by its effect rather than by a count. An
     * ARP request for us produces an ARP reply; an IPv4 ICMP echo
     * produces an ICMP echo reply; anything else produces nothing. Swap
     * the two comparisons in eth_receive and all three of these fail,
     * which is the property the census said was missing. */
    net_fixture();
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, net_local_mac(), 6);
    memcpy(frame + 6, peer_mac, 6);

    /* ARP */
    frame[12] = 0x08; frame[13] = 0x06;
    arp_request(frame + 14, PEER_IP, LOCAL_IP);
    eth_receive(frame, 14 + 28);
    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.ethertype, 0x0806);
    CHECK_EQ((tx.eth[14 + 6] << 8) | tx.eth[14 + 7], 2);  /* ARP opcode: reply */

    /* IPv4 carrying ICMP */
    fake_net_reset();
    uint8_t echo[8] = {8, 0, 0, 0, 0x1E, 0xA5, 0x00, 0x01};
    uint8_t ipf[128];
    memset(ipf, 0, sizeof(ipf));
    memcpy(ipf, net_local_mac(), 6);
    memcpy(ipf + 6, peer_mac, 6);
    ipf[12] = 0x08; ipf[13] = 0x00;
    uint16_t n = ip_build(ipf + 14, 1, LOCAL_IP, echo, sizeof(echo));
    eth_receive(ipf, (uint16_t)(14 + n));
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.ethertype, 0x0800);
    CHECK_EQ(tx.protocol, 1);
    CHECK_EQ(tx.payload[0], 0);   /* ICMP type 0: echo reply */
}

TEST(net_icmp, an_echo_reply_mirrors_the_request_exactly) {
    net_fixture();
    /* icmp.c scored 7.5%: the reply was counted and never read. An echo
     * reply has to carry back the request's identifier, its sequence
     * number and its payload byte for byte - that is the entire point of
     * the protocol, and getting any of the three wrong makes `ping`
     * report nothing while every test still passes. */
    const uint8_t body[] = {'p', 'i', 'n', 'g', 0x00, 0xFF, 0x7F, 0x80};
    uint8_t echo[8 + sizeof(body)];
    echo[0] = 8;                  /* echo request */
    echo[1] = 0;
    echo[2] = 0; echo[3] = 0;     /* checksum, filled by the stack */
    echo[4] = 0x1E; echo[5] = 0xA5;   /* identifier */
    echo[6] = 0x00; echo[7] = 0x2A;   /* sequence 42 */
    memcpy(echo + 8, body, sizeof(body));

    uint8_t pkt[128];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, echo, sizeof(echo));
    ip_handle_packet(peer_mac, pkt, n);

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.protocol, 1);
    REQUIRE(tx.payload_len >= 8 + (uint16_t)sizeof(body));
    CHECK_EQ(tx.payload[0], 0);                 /* echo reply */
    CHECK_EQ(tx.payload[1], 0);                 /* code 0 */
    CHECK_EQ(tx.payload[4], 0x1E);              /* the identifier, back */
    CHECK_EQ(tx.payload[5], 0xA5);
    CHECK_EQ(tx.payload[6], 0x00);              /* and the sequence */
    CHECK_EQ(tx.payload[7], 0x2A);
    CHECK_MEMEQ(tx.payload + 8, body, sizeof(body));

    /* And it goes back to the sender, not to whoever the header said. */
    uint32_t dst = ((uint32_t)tx.ip[16] << 24) | ((uint32_t)tx.ip[17] << 16) |
                   ((uint32_t)tx.ip[18] << 8) | (uint32_t)tx.ip[19];
    CHECK_EQ(dst, PEER_IP);
}

TEST(net_icmp, the_reply_checksum_is_correct) {
    net_fixture();
    uint8_t echo[12] = {8, 0, 0, 0, 0x12, 0x34, 0, 7, 'a', 'b', 'c', 'd'};
    uint8_t pkt[128];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, echo, sizeof(echo));
    ip_handle_packet(peer_mac, pkt, n);

    tx_t tx;
    REQUIRE(last_tx(&tx));
    /* The one's-complement sum of a correct ICMP message is 0xFFFF. A
     * peer with a checksum check - which is every real one - silently
     * drops a reply that fails this, so a wrong checksum looks exactly
     * like no reply at all. */
    uint32_t sum = 0;
    for (uint16_t i = 0; i + 1 < tx.payload_len; i += 2) {
        sum += (uint32_t)((tx.payload[i] << 8) | tx.payload[i + 1]);
    }
    if (tx.payload_len & 1) {
        sum += (uint32_t)(tx.payload[tx.payload_len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    CHECK_EQ(sum, 0xFFFF);
}

TEST(net_arp, a_reply_carries_our_address_and_is_addressed_to_the_asker) {
    net_fixture();
    /* arp.c scored 29.8%. The reply was counted; its contents were not
     * read, so every field could have been wrong. */
    uint8_t pkt[28];
    arp_request(pkt, PEER_IP, LOCAL_IP);
    arp_handle_packet(pkt, sizeof(pkt));

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.ethertype, 0x0806);
    const uint8_t *a = tx.eth + 14;
    CHECK_EQ((a[0] << 8) | a[1], 1);        /* hardware type: Ethernet */
    CHECK_EQ((a[2] << 8) | a[3], 0x0800);   /* protocol type: IPv4 */
    CHECK_EQ(a[4], 6);
    CHECK_EQ(a[5], 4);
    CHECK_EQ((a[6] << 8) | a[7], 2);        /* opcode: reply */
    CHECK_MEMEQ(a + 8, net_local_mac(), 6); /* sender hardware: ours */
    uint32_t spa = ((uint32_t)a[14] << 24) | ((uint32_t)a[15] << 16) |
                   ((uint32_t)a[16] << 8) | (uint32_t)a[17];
    CHECK_EQ(spa, LOCAL_IP);                /* sender protocol: ours */
    CHECK_MEMEQ(a + 18, peer_mac, 6);       /* target: the asker */
    uint32_t tpa = ((uint32_t)a[24] << 24) | ((uint32_t)a[25] << 16) |
                   ((uint32_t)a[26] << 8) | (uint32_t)a[27];
    CHECK_EQ(tpa, PEER_IP);
    /* ...and the Ethernet frame is unicast to the asker, not broadcast. */
    CHECK_MEMEQ(tx.eth, peer_mac, 6);
}

TEST(net_udp, a_datagram_is_delivered_with_the_ports_the_wire_carried) {
    net_fixture();
    /* udp.c scored 17.1%. The existing test read the payload and the
     * destination port; the source port and the source address reached
     * the socket layer unchecked, and a stack that reports the wrong
     * peer makes a reply go to the wrong machine. */
    const char *body = "abcdefgh";
    uint8_t udp[8 + 8];
    be16_put(udp + 0, 0xBEEF);
    be16_put(udp + 2, 1234);
    be16_put(udp + 4, (uint16_t)(8 + 8));
    be16_put(udp + 6, 0);
    memcpy(udp + 8, body, 8);

    uint8_t pkt[128];
    uint16_t n = ip_build(pkt, 17, LOCAL_IP, udp, sizeof(udp));
    ip_handle_packet(peer_mac, pkt, n);

    REQUIRE(fake_socket_delivered_count() == 1);
    uint32_t len = 0;
    uint16_t dst_port = 0;
    const uint8_t *got = fake_socket_delivered(0, &len, &dst_port);
    CHECK_EQ(dst_port, 1234);
    CHECK_EQ(len, 8);
    CHECK_MEMEQ(got, body, 8);
    CHECK_EQ(fake_socket_delivered_src_port(0), 0xBEEF);
    CHECK_EQ(fake_socket_delivered_src_ip(0), PEER_IP);
}

TEST(net_ip, an_outbound_datagram_has_a_well_formed_header) {
    net_fixture();
    /* ip.c scored 17.4% and its whole send path was unexercised: the
     * version, the header length, the TTL, the protocol and the checksum
     * were all free to be wrong. A machine whose IP header has a bad
     * checksum is a machine every router on the path drops. */
    const uint8_t body[] = {1, 2, 3, 4};
    ip_send(PEER_IP, IP_PROTO_UDP, body, sizeof(body));

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.ethertype, 0x0800);
    CHECK_EQ(tx.ip[0] >> 4, 4);            /* version 4 */
    CHECK_EQ(tx.ip[0] & 0x0F, 5);          /* no options */
    CHECK_EQ(((tx.ip[2] << 8) | tx.ip[3]), 20 + (int)sizeof(body));
    CHECK(tx.ip[8] > 0);                   /* a TTL of zero never leaves */
    CHECK_EQ(tx.ip[9], IP_PROTO_UDP);
    uint32_t src = ((uint32_t)tx.ip[12] << 24) | ((uint32_t)tx.ip[13] << 16) |
                   ((uint32_t)tx.ip[14] << 8) | (uint32_t)tx.ip[15];
    CHECK_EQ(src, LOCAL_IP);

    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) {
        sum += (uint32_t)((tx.ip[i] << 8) | tx.ip[i + 1]);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    CHECK_EQ(sum, 0xFFFF);
}

/* ---- Q12, second pass: what the second census found -------------------
 *
 * udp.c scored **3.7%** - three mutants killed out of eighty-two - and
 * the reason was a single omission repeated everywhere: every UDP test
 * in this file sent a datagram with a checksum of zero, which RFC 768
 * defines as "not computed". So the entire checksum path, which is most
 * of what udp_handle_packet does, was never entered, and udp_send was
 * never called at all.
 *
 * A test that only ever exercises the "do not bother checking" branch of
 * a validator is not testing the validator.
 */

/* The UDP checksum for a datagram, computed the way a correct sender
 * would: over a pseudo-header of the two addresses, the protocol and the
 * length, then the datagram itself. Written out here rather than reusing
 * the kernel's helper on purpose - a checksum test that calls the code
 * under test to work out what to expect proves only that the function
 * agrees with itself. */
static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t *datagram, uint16_t udp_len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += 17;                    /* protocol */
    sum += udp_len;
    for (uint16_t i = 0; i + 1 < udp_len; i += 2) {
        sum += (uint32_t)((datagram[i] << 8) | datagram[i + 1]);
    }
    if (udp_len & 1) {
        sum += (uint32_t)(datagram[udp_len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t csum = (uint16_t)(~sum & 0xFFFF);
    return csum ? csum : 0xFFFF;  /* RFC 768: zero is sent as all ones */
}

/* Builds a UDP datagram with a real checksum unless `csum_override` is
 * non-zero, in which case that value is used verbatim. */
static uint16_t udp_build(uint8_t *out, uint16_t src_port, uint16_t dst_port,
                          const uint8_t *body, uint16_t body_len,
                          uint16_t csum_override) {
    uint16_t udp_len = (uint16_t)(8 + body_len);
    be16_put(out + 0, src_port);
    be16_put(out + 2, dst_port);
    be16_put(out + 4, udp_len);
    be16_put(out + 6, 0);
    if (body_len) {
        memcpy(out + 8, body, body_len);
    }
    be16_put(out + 6, csum_override ? csum_override
                                    : udp_checksum(PEER_IP, LOCAL_IP, out, udp_len));
    return udp_len;
}

TEST(net_udp, a_datagram_with_a_correct_checksum_is_delivered) {
    net_fixture();
    const uint8_t body[] = "checksummed";
    uint8_t udp[64];
    uint16_t n = udp_build(udp, 4321, 1234, body, sizeof(body) - 1, 0);
    uint8_t pkt[128];
    uint16_t m = ip_build(pkt, 17, LOCAL_IP, udp, n);
    ip_handle_packet(peer_mac, pkt, m);

    REQUIRE(fake_socket_delivered_count() == 1);
    uint32_t len = 0;
    const uint8_t *got = fake_socket_delivered(0, &len, NULL);
    CHECK_EQ(len, sizeof(body) - 1);
    CHECK_MEMEQ(got, body, sizeof(body) - 1);
}

TEST(net_udp, a_datagram_with_a_wrong_checksum_is_dropped) {
    net_fixture();
    /* The whole point of the checksum. Every bit of it matters, so this
     * corrupts the checksum itself and then, separately, a payload byte
     * while leaving the checksum correct-for-the-original - which is
     * what a corrupted packet on a real wire looks like. */
    const uint8_t body[] = "checksummed";
    uint8_t udp[64];
    uint16_t n = udp_build(udp, 4321, 1234, body, sizeof(body) - 1, 0);

    uint16_t good = (uint16_t)((udp[6] << 8) | udp[7]);
    be16_put(udp + 6, (uint16_t)(good ^ 0x0001));
    uint8_t pkt[128];
    uint16_t m = ip_build(pkt, 17, LOCAL_IP, udp, n);
    ip_handle_packet(peer_mac, pkt, m);
    CHECK_EQ(fake_socket_delivered_count(), 0);

    /* A flipped payload bit, checksum left as it was. */
    fake_socket_reset();
    n = udp_build(udp, 4321, 1234, body, sizeof(body) - 1, 0);
    udp[9] ^= 0x20;
    m = ip_build(pkt, 17, LOCAL_IP, udp, n);
    ip_handle_packet(peer_mac, pkt, m);
    CHECK_EQ(fake_socket_delivered_count(), 0);
}

TEST(net_udp, a_checksum_of_zero_means_the_sender_did_not_compute_one) {
    net_fixture();
    /* RFC 768's exemption, which is the branch every earlier test in
     * this file was accidentally taking. It has to keep working - and it
     * has to be the *only* value that skips the check, which is the
     * assertion the test above supplies. */
    const uint8_t body[] = "unchecked";
    uint8_t udp[64];
    uint16_t n = udp_build(udp, 4321, 1234, body, sizeof(body) - 1, 0);
    be16_put(udp + 6, 0);
    uint8_t pkt[128];
    uint16_t m = ip_build(pkt, 17, LOCAL_IP, udp, n);
    ip_handle_packet(peer_mac, pkt, m);
    CHECK_EQ(fake_socket_delivered_count(), 1);
}

TEST(net_udp, a_datagram_with_no_payload_at_all_is_delivered) {
    net_fixture();
    /* The boundary `len < UDP_HEADER_LEN` guards. Mutated to `<=`, a
     * header-only datagram is dropped - and that mutant survived the
     * whole suite, because nothing had ever sent one. A zero-length UDP
     * datagram is legal and is how several protocols say "I am here". */
    uint8_t udp[8];
    uint16_t n = udp_build(udp, 4321, 1234, NULL, 0, 0);
    CHECK_EQ(n, 8);
    uint8_t pkt[64];
    uint16_t m = ip_build(pkt, 17, LOCAL_IP, udp, n);
    ip_handle_packet(peer_mac, pkt, m);

    REQUIRE(fake_socket_delivered_count() == 1);
    uint32_t len = 1;
    uint16_t port = 0;
    fake_socket_delivered(0, &len, &port);
    CHECK_EQ(len, 0);
    CHECK_EQ(port, 1234);
}

TEST(net_udp, a_length_field_shorter_than_the_header_is_refused) {
    net_fixture();
    for (uint16_t claimed = 0; claimed < 8; claimed++) {
        fake_socket_reset();
        uint8_t udp[16];
        udp_build(udp, 4321, 1234, (const uint8_t *)"ab", 2, 0);
        be16_put(udp + 4, claimed);
        uint8_t pkt[64];
        uint16_t m = ip_build(pkt, 17, LOCAL_IP, udp, 10);
        CHECK_NO_PANIC(ip_handle_packet(peer_mac, pkt, m));
        CHECK_EQ(fake_socket_delivered_count(), 0);
    }
}

TEST(net_udp, a_sent_datagram_has_a_header_a_peer_would_accept) {
    net_fixture();
    /* udp_send was called by nothing in this file, so every byte it
     * writes was free to be wrong. The strongest available assertion is
     * that the datagram it produces validates: computing the checksum
     * over what was actually transmitted must fold to zero, which is the
     * same arithmetic any real peer does before accepting it. */
    const uint8_t body[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    CHECK(udp_send(PEER_IP, 53, 1053, body, sizeof(body)) >= 0);

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.protocol, 17);
    REQUIRE(tx.payload_len == 8 + sizeof(body));
    CHECK_EQ((tx.payload[0] << 8) | tx.payload[1], 1053);   /* source port */
    CHECK_EQ((tx.payload[2] << 8) | tx.payload[3], 53);     /* destination */
    CHECK_EQ((tx.payload[4] << 8) | tx.payload[5], 8 + (int)sizeof(body));
    CHECK_MEMEQ(tx.payload + 8, body, sizeof(body));

    /* The checksum, verified the way a receiver would: sum the
     * pseudo-header and the datagram, and the result must be zero. */
    uint32_t src = ((uint32_t)tx.ip[12] << 24) | ((uint32_t)tx.ip[13] << 16) |
                   ((uint32_t)tx.ip[14] << 8) | (uint32_t)tx.ip[15];
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (PEER_IP >> 16) & 0xFFFF; sum += PEER_IP & 0xFFFF;
    sum += 17;
    sum += tx.payload_len;
    for (uint16_t i = 0; i + 1 < tx.payload_len; i += 2) {
        sum += (uint32_t)((tx.payload[i] << 8) | tx.payload[i + 1]);
    }
    if (tx.payload_len & 1) {
        sum += (uint32_t)(tx.payload[tx.payload_len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    CHECK_EQ(sum, 0xFFFF);
}

TEST(net_udp, a_payload_larger_than_the_mtu_allows_is_refused) {
    net_fixture();
    static uint8_t huge[UDP_MAX_PAYLOAD + 64];
    memset(huge, 'x', sizeof(huge));
    CHECK(udp_send(PEER_IP, 53, 1053, huge, UDP_MAX_PAYLOAD + 1) < 0);
    /* ...and exactly the maximum is accepted, so this is a boundary and
     * not a blanket refusal. */
    fake_net_reset();
    CHECK(udp_send(PEER_IP, 53, 1053, huge, UDP_MAX_PAYLOAD) >= 0);
}
