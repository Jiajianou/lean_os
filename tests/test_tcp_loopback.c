#include "check.h"
#include "fakes/fakes.h"
#include "memory_management/heap.h"

#include "network/tcp.h"

#include <string.h>

#define LOOPBACK 0x7F000001u
#define PORT     5555

static uint8_t pattern(long position) {
    return (uint8_t)('A' + (position * 13 + position / 97) % 26);
}

static void connect_pair(struct tcpcb **client, struct tcpcb **server, struct tcpcb **listener) {
    fake_net_reset();
    kernel_log_capture_reset();
    fake_heap_ensure();
    tcp_init();
    *listener = tcp_open();
    REQUIRE(*listener != NULL);
    CHECK_EQ(tcp_bind(*listener, PORT), PORT);
    CHECK_EQ(tcp_listen(*listener), 0);
    *client = tcp_open();
    REQUIRE(*client != NULL);
    CHECK_EQ(tcp_connect(*client, LOOPBACK, PORT), 0);
    CHECK(tcp_connect_settled(*client));
    CHECK(!tcp_connect_failed(*client));
    CHECK(tcp_accept_pending(*listener));
    *server = tcp_accept(*listener);
    REQUIRE(*server != NULL);
    CHECK_EQ(tcp_state(*client), TCP_ESTABLISHED);
    CHECK_EQ(tcp_state(*server), TCP_ESTABLISHED);
}

TEST(tcp_loopback, sixty_four_KiB_arrives_intact_under_the_syscalls_chunking) {
    struct tcpcb *c, *s, *l;
    connect_pair(&c, &s, &l);
    enum { N = 65536 };
    static uint8_t got[N];
    long sent = 0, recvd = 0;
    int stalls = 0;
    while ((sent < N || recvd < N) && stalls < 100000) {
        int progressed = 0;
        if (sent < N) {
            uint8_t chunk[TCP_MAX_MSS];
            long want = N - sent < TCP_MAX_MSS ? N - sent : TCP_MAX_MSS;
            for (long i = 0; i < want; i++) {
                chunk[i] = pattern(sent + i);
            }
            int m = tcp_send(c, chunk, (uint16_t)want);
            if (m < 0) {
                break;
            }
            if (m > 0) {
                sent += m;
                progressed = 1;
            }
        }
        if (recvd < N) {
            int n = tcp_receive(s, got + recvd, TCP_MAX_MSS);
            if (n < 0) {
                break;
            }
            if (n > 0) {
                recvd += n;
                progressed = 1;
            }
        }
        if (!progressed) {
            tcp_tick();
            stalls++;
        }
    }
    CHECK_EQ(sent, N);
    CHECK_EQ(recvd, N);
    long first_wrong = -1;
    for (long i = 0; i < N; i++) {
        if (got[i] != pattern(i)) {
            first_wrong = i;
            break;
        }
    }
    if (first_wrong >= 0) {
        test_fail(__FILE__, __LINE__, "byte %ld arrived as 0x%02x, wanted 0x%02x",
                  first_wrong, got[first_wrong], pattern(first_wrong));
    }
    tcp_close(c);
    tcp_close(s);
    tcp_release(l);
}

TEST(tcp_loopback, a_receiver_that_reads_late_gets_every_byte_once) {
    struct tcpcb *c, *s, *l;
    connect_pair(&c, &s, &l);
    enum { N = 32768 };
    static uint8_t got[N];
    long sent = 0, recvd = 0;
    int rounds = 0;
    while ((sent < N || recvd < N) && rounds++ < 100000) {
        for (;;) {
            if (sent >= N) break;
            uint8_t chunk[TCP_MAX_MSS];
            long want = N - sent < TCP_MAX_MSS ? N - sent : TCP_MAX_MSS;
            for (long i = 0; i < want; i++) chunk[i] = pattern(sent + i);
            int m = tcp_send(c, chunk, (uint16_t)want);
            CHECK(m >= 0);
            if (m == 0) break;
            sent += m;
        }
        for (;;) {
            if (recvd >= N) break;
            int n = tcp_receive(s, got + recvd, 100);
            CHECK(n >= 0);
            if (n == 0) break;
            recvd += n;
        }
        tcp_tick();
    }
    CHECK_EQ(sent, N);
    CHECK_EQ(recvd, N);
    for (long i = 0; i < N; i++) {
        if (got[i] != pattern(i)) {
            test_fail(__FILE__, __LINE__, "byte %ld arrived as 0x%02x, wanted 0x%02x",
                      i, got[i], pattern(i));
            break;
        }
    }
    tcp_close(c);
    tcp_close(s);
    tcp_release(l);
}

static int pump_receive(struct tcpcb *t, uint8_t *out, int max) {
    for (int stalls = 0; stalls < 1000; stalls++) {
        int n = tcp_receive(t, out, (uint16_t)max);
        if (n != 0) {
            return n;
        }
        tcp_tick();
    }
    return 0;
}

TEST(tcp_loopback, a_half_close_delivers_its_data_then_end_of_file_and_keeps_receiving) {
    struct tcpcb *c, *s, *l;
    connect_pair(&c, &s, &l);
    CHECK_EQ(tcp_send(c, (const uint8_t *)"abc", 3), 3);
    tcp_close(c);
    CHECK_EQ(tcp_state(c), TCP_FIN_WAIT_1);

    uint8_t got[16];
    CHECK_EQ(pump_receive(s, got, sizeof(got)), 3);
    CHECK(memcmp(got, "abc", 3) == 0);
    CHECK(tcp_receive_ended(s));
    CHECK_EQ(pump_receive(s, got, sizeof(got)), -1);

    CHECK(!tcp_receive_ended(c));
    CHECK_EQ(tcp_send(s, (const uint8_t *)"echo:abc", 8), 8);
    CHECK_EQ(pump_receive(c, got, sizeof(got)), 8);
    CHECK(memcmp(got, "echo:abc", 8) == 0);
}

TEST(tcp_loopback, a_peer_fin_with_data_still_queued_ends_only_after_the_data) {
    struct tcpcb *c, *s, *l;
    connect_pair(&c, &s, &l);
    CHECK_EQ(tcp_send(c, (const uint8_t *)"hello", 5), 5);
    tcp_close(c);
    for (int i = 0; i < 50; i++) {
        tcp_tick();
    }
    CHECK(tcp_receive_ended(s));
    CHECK_EQ(tcp_bytes_available(s), 5);
    uint8_t got[16];
    CHECK_EQ(tcp_receive(s, got, sizeof(got)), 5);
    CHECK_EQ(tcp_receive(s, got, sizeof(got)), -1);
}

TEST(tcp_loopback, a_socket_that_never_had_a_peer_has_not_ended) {
    fake_net_reset();
    fake_heap_ensure();
    tcp_init();
    struct tcpcb *fresh = tcp_open();
    REQUIRE(fresh != NULL);
    CHECK(!tcp_receive_ended(fresh));
    struct tcpcb *listener = tcp_open();
    REQUIRE(listener != NULL);
    CHECK_EQ(tcp_bind(listener, PORT), PORT);
    CHECK_EQ(tcp_listen(listener), 0);
    CHECK(!tcp_receive_ended(listener));
}
