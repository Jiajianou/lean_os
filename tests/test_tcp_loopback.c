/* tests/test_tcp_loopback.c - M100: a whole transfer, both ends, on the host.
 *
 * tests/test_tcp_states.c plays the peer by hand, one segment at a time,
 * which is the right instrument for the state machine. This is the other
 * instrument: both ends are this stack, joined by the real loopback queue
 * in kernel/net/ip.c, and what is graded is a transfer - every byte
 * arriving, once, in order - under the exact chunking the machine's
 * syscalls use. M100's seventh increment made read(2) and write(2) on a
 * socket block, and the first program to write 64 KiB in one call and
 * read it from a second thread got 31,836 bytes back and one of them
 * wrong. A boot takes six minutes to say that; this takes a millisecond
 * to say where.
 *
 * The interleaving is the machine's: the sender offers TCP_MAX_MSS at a
 * time and takes what tcp_send accepts (SYS_write's loop), the receiver
 * asks for TCP_MAX_MSS at a time (SYS_read's cap), and they alternate the
 * way two tasks on one core do - a send, then a receive, then a send.
 */
#include "check.h"
#include "fakes/fakes.h"

#include "net/tcp.h"

#include <string.h>

#define LOOPBACK 0x7F000001u
#define PORT     5555

static uint8_t pattern(long pos) {
    return (uint8_t)('A' + (pos * 13 + pos / 97) % 26);
}

/* A connection on loopback: the handshake runs inside tcp_connect,
 * because the queue is drained by the outermost sender. */
static void connect_pair(struct tcpcb **client, struct tcpcb **server, struct tcpcb **listener) {
    fake_net_reset();
    klog_capture_reset();
    tcp_init();
    *listener = tcp_open();
    REQUIRE(*listener != NULL);
    CHECK_EQ(tcp_bind(*listener, PORT), PORT); /* it returns the port it bound */
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
                break; /* the connection is gone; the counts below say so once */
            }
            if (m > 0) {
                sent += m;
                progressed = 1;
            }
        }
        if (recvd < N) {
            int n = tcp_recv(s, got + recvd, TCP_MAX_MSS);
            if (n < 0) {
                break;
            }
            if (n > 0) {
                recvd += n;
                progressed = 1;
            }
        }
        if (!progressed) {
            /* Both sides idle: on the machine the timer runs the
             * retransmit path. Here it is a call. */
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
    /* The other interleaving: the sender fills its buffer and the
     * receiver's window before the receiver reads anything, then the
     * receiver drains in small pieces, then the sender goes again. This
     * is what a reader parked in SYS_read and a writer parked in
     * SYS_write look like when the tick wakes them out of phase. */
    struct tcpcb *c, *s, *l;
    connect_pair(&c, &s, &l);
    enum { N = 32768 };
    static uint8_t got[N];
    long sent = 0, recvd = 0;
    int rounds = 0;
    while ((sent < N || recvd < N) && rounds++ < 100000) {
        /* Push until refused. */
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
        /* Drain in 100-byte reads until empty. */
        for (;;) {
            if (recvd >= N) break;
            int n = tcp_recv(s, got + recvd, 100);
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
