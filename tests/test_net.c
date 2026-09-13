#include "check.h"
#include "fakes/fakes.h"

#include "network/arp.h"
#include "network/ethernet.h"
#include "network/icmp.h"
#include "network/ip.h"
#include "network/net.h"
#include "network/tcp.h"
#include "network/udp.h"
#include "network/net.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PEER_IP   0x0A000202u
#define LOCAL_IP  0x0A00020Fu
#define OTHER_IP  0x0A0002FEu

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

static void arp_request(uint8_t out[28], uint32_t sender_ip, uint32_t target_ip) {
    memset(out, 0, 28);
    be16_put(out + 0, 1);
    be16_put(out + 2, 0x0800);
    out[4] = 6;
    out[5] = 4;
    be16_put(out + 6, 1);
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
    arp_request(pkt, PEER_IP, OTHER_IP);
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);
}

TEST(net_arp, a_truncated_packet_is_dropped_at_every_length) {
    net_fixture();
    uint8_t pkt[28];
    arp_request(pkt, PEER_IP, LOCAL_IP);
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
    be16_put(pkt + 0, 6);
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);

    arp_request(pkt, PEER_IP, LOCAL_IP);
    be16_put(pkt + 2, 0x86DD);
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);
}

static void arp_reply(uint8_t out[28], uint32_t sender_ip, const uint8_t mac[6]) {
    arp_request(out, sender_ip, LOCAL_IP);
    be16_put(out + 6, 2);
    memcpy(out + 8, mac, 6);
}

TEST(net_arp, the_first_packet_to_an_unresolved_neighbour_is_sent_when_it_answers) {
    net_fixture();
    arp_init();
    const uint32_t NEIGHBOUR = 0x0A00024Du;
    static const uint8_t neighbour_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x4D};
    const uint8_t payload[] = "held, not dropped";

    CHECK_EQ(ip_send(NEIGHBOUR, 253, payload, sizeof(payload)), 0);
    REQUIRE(fake_net_tx_count() >= 1);
    for (int i = 0; i < fake_net_tx_count(); i++) {
        uint32_t len = 0;
        const uint8_t *f = fake_net_tx_frame(i, &len);
        CHECK_EQ((f[12] << 8) | f[13], 0x0806);
    }

    fake_net_reset();
    uint8_t pkt[28];
    arp_reply(pkt, NEIGHBOUR, neighbour_mac);
    arp_handle_packet(pkt, sizeof(pkt));

    REQUIRE(fake_net_tx_count() == 1);
    uint32_t len = 0;
    const uint8_t *f = fake_net_tx_frame(0, &len);
    CHECK_MEMEQ(f, neighbour_mac, 6);
    CHECK_EQ((f[12] << 8) | f[13], 0x0800);
    CHECK_EQ(f[14 + 9], 253);
    CHECK_EQ(((uint32_t)f[14 + 16] << 24) | ((uint32_t)f[14 + 17] << 16) |
             ((uint32_t)f[14 + 18] << 8) | f[14 + 19], NEIGHBOUR);
    CHECK_MEMEQ(f + 14 + 20, payload, sizeof(payload));

    fake_net_reset();
    arp_handle_packet(pkt, sizeof(pkt));
    CHECK_EQ(fake_net_tx_count(), 0);
}

TEST(net_arp, a_held_packet_is_replaced_by_a_newer_one_and_released_only_by_its_own_neighbour) {
    net_fixture();
    arp_init();
    const uint32_t A = 0x0A000250u, B = 0x0A000251u;
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

TEST(net_arp, a_neighbour_that_never_answers_is_unreachable_by_the_third_send) {
    net_fixture();
    arp_init();
    const uint32_t SILENT = 0x0A0002EEu;
    CHECK_EQ(ip_send(SILENT, 253, (const uint8_t *)"1", 1), 0);
    CHECK_EQ(ip_send(SILENT, 253, (const uint8_t *)"2", 1), 0);
    CHECK_EQ(ip_send(SILENT, 253, (const uint8_t *)"3", 1), -1);
    CHECK_EQ(ip_send(SILENT, 253, (const uint8_t *)"4", 1), 0);
}

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
    be16_put(frame + 12, 0x88CC);
    CHECK_NO_PANIC(eth_receive(frame, sizeof(frame)));
    CHECK_EQ(fake_net_tx_count(), 0);
}

static uint16_t ip_build(uint8_t *out, uint8_t proto, uint32_t dst,
                         const uint8_t *payload, uint16_t payload_len) {
    memset(out, 0, 20);
    out[0] = 0x45;
    be16_put(out + 2, (uint16_t)(20 + payload_len));
    out[8] = 64;
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
    uint8_t icmp[8] = {8, 0, 0, 0, 0x12, 0x34, 0, 1};
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1  , OTHER_IP, icmp, sizeof(icmp));
    ip_handle_packet(peer_mac, pkt, n);
    CHECK_EQ(fake_net_tx_count(), 0);
}

TEST(net_ip, a_header_claiming_a_length_longer_than_the_frame_is_not_believed) {
    net_fixture();
    uint8_t icmp[8] = {8, 0, 0, 0, 0x12, 0x34, 0, 1};
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, icmp, sizeof(icmp));

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

TEST(net_icmp, an_echo_request_is_answered_and_a_truncated_one_is_not) {
    net_fixture();
    uint8_t echo[12] = {8, 0, 0, 0, 0x1E, 0xA5, 0x00, 0x01, 'd', 'a', 't', 'a'};
    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, echo, sizeof(echo));
    ip_handle_packet(peer_mac, pkt, n);
    CHECK_EQ(fake_net_tx_count(), 1);

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

TEST(net_udp, a_well_formed_datagram_reaches_the_socket_layer_intact) {
    net_fixture();
    const char *body = "hello over udp";
    uint16_t blen = (uint16_t)strlen(body);
    uint8_t udp[8 + 32];
    be16_put(udp + 0, 4321);
    be16_put(udp + 2, 1234);
    be16_put(udp + 4, (uint16_t)(8 + blen));
    be16_put(udp + 6, 0);
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
    be16_put(udp + 4, 1400);
    be16_put(udp + 6, 0);
    memcpy(udp + 8, "abcd", 4);

    uint8_t pkt[64];
    uint16_t n = ip_build(pkt, 17, LOCAL_IP, udp, sizeof(udp));
    uint8_t *heap = malloc(n);
    REQUIRE(heap != NULL);
    memcpy(heap, pkt, n);
    CHECK_NO_PANIC(ip_handle_packet(peer_mac, heap, n));

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
    for (uint8_t off = 5; off <= 15; off++) {
        uint8_t seg[20];
        memset(seg, 0, sizeof(seg));
        be16_put(seg + 0, 12345);
        be16_put(seg + 2, 80);
        seg[12] = (uint8_t)(off << 4);
        seg[13] = 0x02;
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
    const uint8_t bad_lens[] = {0, 1, 200, 255};
    for (unsigned i = 0; i < sizeof(bad_lens); i++) {
        uint8_t seg[24];
        memset(seg, 0, sizeof(seg));
        be16_put(seg + 0, 12345);
        be16_put(seg + 2, 80);
        seg[12] = 6 << 4;
        seg[13] = 0x02;
        be16_put(seg + 14, 1024);
        seg[20] = 2;
        seg[21] = bad_lens[i];
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
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22};
    eth_send(peer_mac, 0x0800, payload, sizeof(payload));

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_MEMEQ(tx.eth, peer_mac, 6);
    CHECK_MEMEQ(tx.eth + 6, net_local_mac(), 6);
    CHECK_EQ(tx.eth[12], 0x08);
    CHECK_EQ(tx.eth[13], 0x00);
    CHECK_MEMEQ(tx.eth + 14, payload, sizeof(payload));
}

TEST(net_eth, a_short_frame_is_padded_to_the_minimum_and_the_padding_is_zero) {
    net_fixture();
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
    net_fixture();
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, net_local_mac(), 6);
    memcpy(frame + 6, peer_mac, 6);

    frame[12] = 0x08; frame[13] = 0x06;
    arp_request(frame + 14, PEER_IP, LOCAL_IP);
    eth_receive(frame, 14 + 28);
    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.ethertype, 0x0806);
    CHECK_EQ((tx.eth[14 + 6] << 8) | tx.eth[14 + 7], 2);

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
    CHECK_EQ(tx.payload[0], 0);
}

TEST(net_icmp, an_echo_reply_mirrors_the_request_exactly) {
    net_fixture();
    const uint8_t body[] = {'p', 'i', 'n', 'g', 0x00, 0xFF, 0x7F, 0x80};
    uint8_t echo[8 + sizeof(body)];
    echo[0] = 8;
    echo[1] = 0;
    echo[2] = 0; echo[3] = 0;
    echo[4] = 0x1E; echo[5] = 0xA5;
    echo[6] = 0x00; echo[7] = 0x2A;
    memcpy(echo + 8, body, sizeof(body));

    uint8_t pkt[128];
    uint16_t n = ip_build(pkt, 1, LOCAL_IP, echo, sizeof(echo));
    ip_handle_packet(peer_mac, pkt, n);

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.protocol, 1);
    REQUIRE(tx.payload_len >= 8 + (uint16_t)sizeof(body));
    CHECK_EQ(tx.payload[0], 0);
    CHECK_EQ(tx.payload[1], 0);
    CHECK_EQ(tx.payload[4], 0x1E);
    CHECK_EQ(tx.payload[5], 0xA5);
    CHECK_EQ(tx.payload[6], 0x00);
    CHECK_EQ(tx.payload[7], 0x2A);
    CHECK_MEMEQ(tx.payload + 8, body, sizeof(body));

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
    uint8_t pkt[28];
    arp_request(pkt, PEER_IP, LOCAL_IP);
    arp_handle_packet(pkt, sizeof(pkt));

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.ethertype, 0x0806);
    const uint8_t *a = tx.eth + 14;
    CHECK_EQ((a[0] << 8) | a[1], 1);
    CHECK_EQ((a[2] << 8) | a[3], 0x0800);
    CHECK_EQ(a[4], 6);
    CHECK_EQ(a[5], 4);
    CHECK_EQ((a[6] << 8) | a[7], 2);
    CHECK_MEMEQ(a + 8, net_local_mac(), 6);
    uint32_t spa = ((uint32_t)a[14] << 24) | ((uint32_t)a[15] << 16) |
                   ((uint32_t)a[16] << 8) | (uint32_t)a[17];
    CHECK_EQ(spa, LOCAL_IP);
    CHECK_MEMEQ(a + 18, peer_mac, 6);
    uint32_t tpa = ((uint32_t)a[24] << 24) | ((uint32_t)a[25] << 16) |
                   ((uint32_t)a[26] << 8) | (uint32_t)a[27];
    CHECK_EQ(tpa, PEER_IP);
    CHECK_MEMEQ(tx.eth, peer_mac, 6);
}

TEST(net_udp, a_datagram_is_delivered_with_the_ports_the_wire_carried) {
    net_fixture();
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
    const uint8_t body[] = {1, 2, 3, 4};
    ip_send(PEER_IP, IP_PROTO_UDP, body, sizeof(body));

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.ethertype, 0x0800);
    CHECK_EQ(tx.ip[0] >> 4, 4);
    CHECK_EQ(tx.ip[0] & 0x0F, 5);
    CHECK_EQ(((tx.ip[2] << 8) | tx.ip[3]), 20 + (int)sizeof(body));
    CHECK(tx.ip[8] > 0);
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

static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t *datagram, uint16_t udp_len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += 17;
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
    return csum ? csum : 0xFFFF;
}

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
    const uint8_t body[] = "checksummed";
    uint8_t udp[64];
    uint16_t n = udp_build(udp, 4321, 1234, body, sizeof(body) - 1, 0);

    uint16_t good = (uint16_t)((udp[6] << 8) | udp[7]);
    be16_put(udp + 6, (uint16_t)(good ^ 0x0001));
    uint8_t pkt[128];
    uint16_t m = ip_build(pkt, 17, LOCAL_IP, udp, n);
    ip_handle_packet(peer_mac, pkt, m);
    CHECK_EQ(fake_socket_delivered_count(), 0);

    fake_socket_reset();
    n = udp_build(udp, 4321, 1234, body, sizeof(body) - 1, 0);
    udp[9] ^= 0x20;
    m = ip_build(pkt, 17, LOCAL_IP, udp, n);
    ip_handle_packet(peer_mac, pkt, m);
    CHECK_EQ(fake_socket_delivered_count(), 0);
}

TEST(net_udp, a_checksum_of_zero_means_the_sender_did_not_compute_one) {
    net_fixture();
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
    const uint8_t body[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    CHECK(udp_send(PEER_IP, 53, 1053, body, sizeof(body)) >= 0);

    tx_t tx;
    REQUIRE(last_tx(&tx));
    CHECK_EQ(tx.protocol, 17);
    REQUIRE(tx.payload_len == 8 + sizeof(body));
    CHECK_EQ((tx.payload[0] << 8) | tx.payload[1], 1053);
    CHECK_EQ((tx.payload[2] << 8) | tx.payload[3], 53);
    CHECK_EQ((tx.payload[4] << 8) | tx.payload[5], 8 + (int)sizeof(body));
    CHECK_MEMEQ(tx.payload + 8, body, sizeof(body));

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
    fake_net_reset();
    CHECK(udp_send(PEER_IP, 53, 1053, huge, UDP_MAX_PAYLOAD) >= 0);
}
