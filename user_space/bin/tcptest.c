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
#include <arpa/inet.h>  /* M100: the POSIX section below */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

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

/* M100: the other end of the POSIX section, on its own thread. Writes a
 * late message so the main thread's read has to block for it, reads 64
 * KiB checking every byte against the pattern the writer used, answers,
 * and closes - which is what turns the main thread's last read into 0. */
/* 16 KiB, the size the native section above already streams fast and
 * reliably: enough to block the writer several times over a 4 KiB send
 * buffer and to exercise the reentrant-try_send path M100 fixed, without
 * the loop-queue-overflowing burst a 64 KiB transfer sets off on the
 * synchronous loopback path (LOOP_QUEUE_DEPTH is 32, and RTO recovery
 * from a dropped burst is minutes on TCG). */
#define POSIX_BIG 16384
static void *posix_peer(void *arg) {
    int fd = *(int *)arg;
    usleep(200000);
    if (write(fd, "late", 4) != 4) {
        return (void *)1;
    }
    long got = 0;
    while (got < POSIX_BIG) {
        char chunk[2048];
        long n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            /* A read that ends early is a different fault from a byte
             * that is wrong, and the main thread's assertion cannot tell
             * them apart - so the peer says which, with how far it got. */
            printf("tcptest: posix peer: read returned %ld after %ld of %d bytes\n",
                   n, got, POSIX_BIG);
            return (void *)1;
        }
        for (long i = 0; i < n; i++) {
            long pos = got + i;
            if (chunk[i] != (char)('A' + (pos * 13 + pos / 97) % 26)) {
                printf("tcptest: posix peer: byte %ld arrived as 0x%02x, wanted 0x%02x\n",
                       pos, (unsigned char)chunk[i], (unsigned char)('A' + (pos * 13 + pos / 97) % 26));
                return (void *)1;
            }
        }
        got += n;
    }
    if (write(fd, "done", 4) != 4) {
        return (void *)1;
    }
    /* Deliberately does NOT close: a thread here gets a COPY of the fd
     * table (the divergence <pthread.h> documents), so this close would
     * drop only the peer's reference and the server socket would stay
     * open on the main thread - no FIN, and main's read-for-EOF below
     * would block forever. Main owns the close, after the join drops
     * this copy. The first version closed here and hung exactly so. */
    return (void *)0;
}

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

    /* ---- M100: the POSIX calls, and they block ------------------------
     *
     * Everything above is this ABI's own calls, none of which ever
     * blocks. A program written against POSIX calls read() and expects
     * to wait, calls write() with 64 KiB and expects 64 KiB back, sets
     * O_NONBLOCK and expects EAGAIN. mbedtls's socket layer is exactly
     * that program. The peer is a thread, because a blocking read needs
     * somebody else to do the writing, and the checks are on the clock
     * as well as the bytes: a read that "blocked" for zero milliseconds
     * before returning what arrived 200 ms later did not block. */
    {
        int lis = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT + 1);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(lis >= 0 && bind(lis, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
              listen(lis, 1) == 0, "POSIX socket/bind/listen failed");
        int cli = socket(AF_INET, SOCK_STREAM, 0);
        check(cli >= 0 && connect(cli, (struct sockaddr *)&addr, sizeof(addr)) == 0,
              "POSIX connect failed");
        check(wait_until(has_pending, lis, 2000), "the POSIX listener never saw the connection");
        int srv = accept(lis, 0, 0);
        check(srv >= 0, "POSIX accept returned nothing after the handshake");

        pthread_t peer;
        check(pthread_create(&peer, 0, posix_peer, &srv) == 0, "could not start the peer thread");
        /* Progress lines, printed as the section goes: a hang here is a
         * hang in a blocking call, and the line before it is the one
         * that names the call. */
        printf("tcptest: posix: connected, waiting on a blocking read\n");

        /* 1. A blocking read waits for the peer's late message. */
        char buf[64];
        memset(buf, 0, sizeof(buf));
        long t0 = sys_uptime_ms();
        long n = read(cli, buf, sizeof(buf));
        long waited = sys_uptime_ms() - t0;
        check(n == 4 && memcmp(buf, "late", 4) == 0, "a blocking read did not return the peer's message");
        check(waited >= 100, "a blocking read returned before the peer had written");
        printf("tcptest: posix: the blocking read returned after %ld ms\n", waited);

        /* 2. O_NONBLOCK: nothing pending is EAGAIN, not a wait and not 0. */
        check(fcntl(cli, F_SETFL, O_NONBLOCK) == 0 && (fcntl(cli, F_GETFL) & O_NONBLOCK),
              "O_NONBLOCK could not be set on a socket");
        errno = 0;
        check(read(cli, buf, sizeof(buf)) == -1 && errno == EAGAIN,
              "a non-blocking read with nothing pending was not EAGAIN");
        errno = 0;
        check(recv(cli, buf, sizeof(buf), MSG_DONTWAIT) == -1 && errno == EAGAIN,
              "recv(MSG_DONTWAIT) with nothing pending was not EAGAIN");
        check(fcntl(cli, F_SETFL, 0) == 0, "O_NONBLOCK could not be cleared");

        /* 3. One write of 64 KiB - forty-five segments through a 4 KiB
         *    send buffer - returns 64 KiB, because the call parks while
         *    the peer drains. The peer checks every byte. */
        static char big[POSIX_BIG];
        for (int i = 0; i < POSIX_BIG; i++) {
            big[i] = (char)('A' + (i * 13 + i / 97) % 26);
        }
        printf("tcptest: posix: O_NONBLOCK answered EAGAIN, writing 16 KiB\n");
        long put = write(cli, big, POSIX_BIG);
        check(put == POSIX_BIG, "one write() did not return every byte it was given");
        printf("tcptest: posix: write returned %ld, waiting for the peer's confirmation\n", put);
        memset(buf, 0, sizeof(buf));
        n = read(cli, buf, sizeof(buf));
        check(n == 4 && memcmp(buf, "done", 4) == 0, "the peer did not confirm the transfer arrived intact");

        /* 4. Join first, so the peer's COPY of the server fd is released
         *    (a thread's fd table is copied, not shared), then close the
         *    main thread's own reference - now the last one, so the FIN
         *    goes out - and the client's read sees the end of the
         *    stream. Getting this order wrong is a hang, not a wrong
         *    answer, which is why it is spelled out. */
        void *peer_result = (void *)1;
        check(pthread_join(peer, &peer_result) == 0 && peer_result == (void *)0,
              "the peer thread saw a byte out of place");
        close(srv);
        n = read(cli, buf, sizeof(buf));
        check(n == 0, "read() after the server closed was not 0");
        printf("tcptest: posix: end of stream seen\n");
        close(cli);
        close(lis);
    }

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
