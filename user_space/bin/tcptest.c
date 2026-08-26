/* user_space/bin/tcptest.c
 *
 * M66's self-test. Both ends of every connection here are this stack
 * talking to itself over loopback, which is not a weaker test than
 * talking to somebody else's TCP - it is a different one, and it is the
 * one that can run on a machine with no network at all. The state
 * machine, the sequence arithmetic, the window, the retransmission timer
 * and the congestion window are all exercised twice per connection,
 * once from each side, and a mistake in any of them shows up as a
 * transfer that stalls or comes back wrong.
 *
 * What loopback cannot do is lose a segment, and "retransmission is
 * untested" is precisely the thing that is true of most from-scratch TCP
 * implementations and never written down. So the boot self-test asks the
 * kernel to drop the next few segments before the big transfer, and this
 * program asserts the transfer completes anyway - the retransmission
 * count comes back in the kernel's own log line.
 *
 * Everything is polled against a deadline, never spun on forever: this
 * runs during boot, and a test that can hang is a machine that can fail
 * to start.
 */
#include <stdio.h>
#include <string.h>

#include "os_net.h"
#include "syscall_wrappers.h"

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("tcptest: FAILED - %s\n", what);
        failures++;
    }
}

#define LOOPBACK  OS_IPV4(127, 0, 0, 1)
#define PORT      8080
#define DEAD_PORT 8099

/* Polls `fn`'s condition until it holds or the deadline passes. Returns
 * whether it held. sys_yield rather than a spin: the other end of every
 * connection here is the kernel's own timer thread and the NIC path, and
 * a program that never yields is a program those never run behind. */
static int wait_until(int (*ready)(int), int fd, uint32_t ms) {
    long deadline = sys_uptime_ms() + (long)ms;
    while (sys_uptime_ms() < deadline) {
        if (ready(fd)) {
            return 1;
        }
        sys_yield();
    }
    return ready(fd);
}

static int connected(int fd)    { return sys_connstat(fd) != 0; }
static int has_pending(int fd)  { return sys_sockpoll(fd) > 0; }

int main(void) {
    /* ---- a connection, both ends of it ------------------------------- */
    int listener = (int)sys_socket(OS_SOCK_STREAM);
    check(listener >= 0, "could not create a stream socket");
    check(sys_bind(listener, PORT) == PORT, "bind did not return the port it bound");
    check(sys_listen(listener) == 0, "listen failed");

    int client = (int)sys_socket(OS_SOCK_STREAM);
    check(sys_connect(client, LOOPBACK, PORT) == 0, "connect did not start");

    /* On loopback this is already established by the time sys_connect
     * returns - the whole handshake runs inside the queue drain. The
     * wait is here because that is an implementation detail of loopback
     * and not something a program should be written to assume. */
    check(wait_until(connected, client, 5000), "the handshake never settled");
    check(sys_connstat(client) == 1, "the connection was not established");

    check(wait_until(has_pending, listener, 2000), "the listener never saw the connection");
    os_sockaddr_t peer;
    int server = (int)sys_accept(listener, &peer);
    check(server >= 0, "accept returned nothing after the handshake completed");
    check(peer.ip == LOOPBACK, "accept reported the wrong peer address");

    /* ---- a short message each way ------------------------------------- */
    const char *hello = "hello from the client";
    check(sys_send(client, hello, (uint32_t)strlen(hello)) == (long)strlen(hello),
          "a short send did not take the whole message");

    /* Six seconds, not two, and the reason is the boot self-test that
     * runs this: it has told the kernel to throw the next two data
     * segments away, and go-back-N means the second drop lands on the
     * *retransmission* of the first. So this message arrives on the
     * second retry, after the RTO has backed off once - 1s, then 2s.
     * A deadline tight enough to fail on the backoff would be a test
     * that only passes when nothing goes wrong. */
    check(wait_until(has_pending, server, 6000), "the server never received the message");
    char buf[64];
    memset(buf, 0, sizeof(buf));
    long n = sys_recv(server, buf, sizeof(buf));
    check(n == (long)strlen(hello), "the server read the wrong length");
    check(memcmp(buf, hello, strlen(hello)) == 0, "the message changed in transit");

    const char *back = "and hello back";
    check(sys_send(server, back, (uint32_t)strlen(back)) == (long)strlen(back),
          "the reply did not send");
    check(wait_until(has_pending, client, 6000), "the client never received the reply");
    memset(buf, 0, sizeof(buf));
    n = sys_recv(client, buf, sizeof(buf));
    check(n == (long)strlen(back), "the client read the wrong length");
    check(memcmp(buf, back, strlen(back)) == 0, "the reply changed in transit");

    /* ---- a transfer big enough to segment ----------------------------- */
    /* 16 KiB through a 4 KiB send buffer over a 1460-byte MSS: this is
     * the only part of the test that exercises the window, the
     * congestion window, buffer compaction on ACK, and the sender
     * blocking and resuming. The pattern is position-dependent so a
     * transfer that arrives in the wrong order or drops a segment
     * silently is caught by content and not only by length. */
    enum { BIG = 16384 };
    static char sent[BIG];
    static char got[BIG];
    for (int i = 0; i < BIG; i++) {
        sent[i] = (char)('a' + (i * 7 + i / 251) % 26);
    }

    int off = 0, in = 0;
    long deadline = sys_uptime_ms() + 20000;
    while ((off < BIG || in < BIG) && sys_uptime_ms() < deadline) {
        if (off < BIG) {
            long put = sys_send(client, sent + off, (uint32_t)(BIG - off));
            if (put > 0) {
                off += (int)put;
            }
        }
        long take = sys_recv(server, got + in, (uint32_t)(BIG - in));
        if (take > 0) {
            in += (int)take;
        }
        sys_yield();
    }
    check(off == BIG, "the big transfer never finished sending");
    check(in == BIG, "the big transfer never finished arriving");
    check(memcmp(sent, got, BIG) == 0, "the big transfer arrived corrupted");

    /* ---- closing, and the end-of-stream a reader needs ----------------- */
    sys_close(client);
    long eof_deadline = sys_uptime_ms() + 3000;
    long r = 0;
    while (sys_uptime_ms() < eof_deadline) {
        r = sys_recv(server, buf, sizeof(buf));
        if (r < 0) {
            break;
        }
        sys_yield();
    }
    check(r < 0, "the server never saw the end of the stream after the client closed");
    sys_close(server);
    sys_close(listener);

    /* ---- a connection nobody is listening for -------------------------- */
    /* This must fail, and it must fail *quickly* - the RST comes back
     * immediately, and a stack that waited for its connect timeout here
     * would make every mistyped port a ten-second pause. */
    int refused = (int)sys_socket(OS_SOCK_STREAM);
    check(sys_connect(refused, LOOPBACK, DEAD_PORT) == 0, "connect to a dead port did not start");
    long refuse_start = sys_uptime_ms();
    check(wait_until(connected, refused, 3000), "connect to a dead port never settled");
    check(sys_connstat(refused) < 0, "connecting to a port nobody listens on succeeded");
    check(sys_uptime_ms() - refuse_start < 2000,
          "a refused connection took a timeout instead of an RST");
    sys_close(refused);

    /* ---- the ways the API is supposed to fail --------------------------- */
    int dgram = (int)sys_socket(OS_SOCK_DGRAM);
    check(sys_listen(dgram) < 0, "listen on a datagram socket succeeded");
    check(sys_send(dgram, "x", 1) < 0, "send on a datagram socket succeeded");
    check(sys_recv(dgram, buf, 1) < 0, "recv on a datagram socket succeeded");
    check(sys_accept(dgram, 0) < 0, "accept on a datagram socket succeeded");
    sys_close(dgram);

    check(sys_socket(7) < 0, "sys_socket accepted a socket type that does not exist");
    check(sys_send(1, "x", 1) < 0, "send on stdout succeeded");
    check(sys_connstat(0) < 0, "connstat on stdin succeeded");

    if (failures) {
        printf("tcptest: %d check(s) failed\n", failures);
        return 1;
    }
    printf("tcptest: all checks passed\n");
    return 0;
}
