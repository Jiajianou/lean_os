#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "os_network.h"
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
            printf("tcptest: posix peer: read returned %ld after %ld of %d bytes\n",
                   n, got, POSIX_BIG);
            return (void *)1;
        }
        for (long i = 0; i < n; i++) {
            long position = got + i;
            if (chunk[i] != (char)('A' + (position * 13 + position / 97) % 26)) {
                printf("tcptest: posix peer: byte %ld arrived as 0x%02x, wanted 0x%02x\n",
                       position, (unsigned char)chunk[i], (unsigned char)('A' + (position * 13 + position / 97) % 26));
                return (void *)1;
            }
        }
        got += n;
    }
    if (write(fd, "done", 4) != 4) {
        return (void *)1;
    }
    return (void *)0;
}

int main(void) {
    int listener = (int)sys_socket(OS_SOCKET_STREAM);
    check(listener >= 0, "could not create a stream socket");
    check(sys_bind(listener, PORT) == PORT, "bind did not return the port it bound");
    check(sys_listen(listener) == 0, "listen failed");

    int client = (int)sys_socket(OS_SOCKET_STREAM);
    check(sys_connect(client, LOOPBACK, PORT) == 0, "connect did not start");

    check(wait_until(connected, client, 5000), "the handshake never settled");
    check(sys_connstat(client) == 1, "the connection was not established");

    check(wait_until(has_pending, listener, 2000), "the listener never saw the connection");
    os_sockaddr_t peer;
    int server = (int)sys_accept(listener, &peer);
    check(server >= 0, "accept returned nothing after the handshake completed");
    check(peer.ip == LOOPBACK, "accept reported the wrong peer address");

    const char *hello = "hello from the client";
    check(sys_send(client, hello, (uint32_t)strlen(hello)) == (long)strlen(hello),
          "a short send did not take the whole message");

    check(wait_until(has_pending, server, 6000), "the server never received the message");
    char buffer[64];
    memset(buffer, 0, sizeof(buffer));
    long n = sys_receive(server, buffer, sizeof(buffer));
    check(n == (long)strlen(hello), "the server read the wrong length");
    check(memcmp(buffer, hello, strlen(hello)) == 0, "the message changed in transit");

    const char *back = "and hello back";
    check(sys_send(server, back, (uint32_t)strlen(back)) == (long)strlen(back),
          "the reply did not send");
    check(wait_until(has_pending, client, 6000), "the client never received the reply");
    memset(buffer, 0, sizeof(buffer));
    n = sys_receive(client, buffer, sizeof(buffer));
    check(n == (long)strlen(back), "the client read the wrong length");
    check(memcmp(buffer, back, strlen(back)) == 0, "the reply changed in transit");

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
        long take = sys_receive(server, got + in, (uint32_t)(BIG - in));
        if (take > 0) {
            in += (int)take;
        }
        sys_yield();
    }
    check(off == BIG, "the big transfer never finished sending");
    check(in == BIG, "the big transfer never finished arriving");
    check(memcmp(sent, got, BIG) == 0, "the big transfer arrived corrupted");

    sys_close(client);
    long eof_deadline = sys_uptime_ms() + 3000;
    long r = 0;
    while (sys_uptime_ms() < eof_deadline) {
        r = sys_receive(server, buffer, sizeof(buffer));
        if (r < 0) {
            break;
        }
        sys_yield();
    }
    check(r < 0, "the server never saw the end of the stream after the client closed");
    sys_close(server);
    sys_close(listener);

    int refused = (int)sys_socket(OS_SOCKET_STREAM);
    check(sys_connect(refused, LOOPBACK, DEAD_PORT) == 0, "connect to a dead port did not start");
    long refuse_start = sys_uptime_ms();
    check(wait_until(connected, refused, 3000), "connect to a dead port never settled");
    check(sys_connstat(refused) < 0, "connecting to a port nobody listens on succeeded");
    check(sys_uptime_ms() - refuse_start < 2000,
          "a refused connection took a timeout instead of an RST");
    sys_close(refused);

    {
        int lis = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(PORT + 1);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(lis >= 0 && bind(lis, (struct sockaddr *)&address, sizeof(address)) == 0 &&
              listen(lis, 1) == 0, "POSIX socket/bind/listen failed");
        /* M151: which port a socket is on, asked of the socket rather than
           of the machine. A listener bound to a port it named must report
           that port back, and a socket bound to port zero must report the
           one the kernel chose - which is the whole reason a server that
           does not want a fixed port can tell its clients where it is. */
        struct sockaddr_in named;
        socklen_t named_length = sizeof(named);
        memset(&named, 0, sizeof(named));
        check(getsockname(lis, (struct sockaddr *)&named, &named_length) == 0 &&
              ntohs(named.sin_port) == PORT + 1,
              "getsockname did not report the port the listener bound");

        int ephemeral = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in any;
        memset(&any, 0, sizeof(any));
        any.sin_family = AF_INET;
        any.sin_port = 0;
        any.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(ephemeral >= 0 &&
              bind(ephemeral, (struct sockaddr *)&any, sizeof(any)) == 0,
              "binding to port zero failed");
        memset(&named, 0, sizeof(named));
        named_length = sizeof(named);
        check(getsockname(ephemeral, (struct sockaddr *)&named,
                          &named_length) == 0 &&
              ntohs(named.sin_port) != 0 &&
              ntohs(named.sin_port) != PORT + 1,
              "getsockname did not report the port a bind to zero was given");
        printf("tcptest: posix: a bind to port 0 was given %u\n",
               (unsigned)ntohs(named.sin_port));
        close(ephemeral);

        int cli = socket(AF_INET, SOCK_STREAM, 0);
        check(cli >= 0 && connect(cli, (struct sockaddr *)&address, sizeof(address)) == 0,
              "POSIX connect failed");
        check(wait_until(has_pending, lis, 2000), "the POSIX listener never saw the connection");
        int srv = accept(lis, 0, 0);
        check(srv >= 0, "POSIX accept returned nothing after the handshake");

        /* MSG_PEEK, which is a question and not a read. This is the exact
           thing net::SocketPosix does before it reuses a socket - peek one
           byte, and take EAGAIN to mean "still there and nothing on it" -
           and until M183 this libc forwarded MSG_PEEK to read(2), so that
           one byte left the stream for good. On a TLS connection that is a
           record header read one byte late, which is a fatal alert, which
           is why no https page would load in the browser. The check is that
           the stream is still whole afterwards. */
        {
            static char blob[300];
            static char back[300];
            for (int i = 0; i < (int)sizeof(blob); i++) {
                blob[i] = (char)(0x40 + (i * 7) % 60);
            }
            check(fcntl(cli, F_SETFL, O_NONBLOCK) == 0,
                  "O_NONBLOCK could not be set before the peek checks");
            char c = 0;
            errno = 0;
            check(recv(cli, &c, 1, MSG_PEEK) == -1 && errno == EAGAIN,
                  "a peek at a live connection with nothing on it was not EAGAIN");
            check(write(srv, blob, sizeof(blob)) == (long)sizeof(blob),
                  "the peer could not write the bytes to peek at");
            check(wait_until(has_pending, cli, 4000),
                  "the bytes to peek at never arrived");
            c = 0;
            check(recv(cli, &c, 1, MSG_PEEK) == 1 && c == blob[0],
                  "a peek did not report the first byte of the stream");
            c = 0;
            check(recv(cli, &c, 1, MSG_PEEK) == 1 && c == blob[0],
                  "a second peek saw a different byte - the first one consumed it");
            long got = 0;
            long deadline = sys_uptime_ms() + 4000;
            while (got < (long)sizeof(back) && sys_uptime_ms() < deadline) {
                errno = 0;
                long n = read(cli, back + got, sizeof(back) - (size_t)got);
                if (n > 0) {
                    got += n;
                } else if (n < 0 && errno == EAGAIN) {
                    sys_yield();
                } else {
                    break;
                }
            }
            check(got == (long)sizeof(blob),
                  "the stream was short after a peek - the peek consumed bytes (M183)");
            check(got == (long)sizeof(blob) && memcmp(back, blob, sizeof(blob)) == 0,
                  "the bytes after a peek were not the bytes that were sent (M183)");
            printf("tcptest: posix: %ld bytes read back whole after two peeks\n", got);
            check(fcntl(cli, F_SETFL, 0) == 0,
                  "O_NONBLOCK could not be cleared after the peek checks");
        }

        pthread_t peer;
        check(pthread_create(&peer, 0, posix_peer, &srv) == 0, "could not start the peer thread");
        printf("tcptest: posix: connected, waiting on a blocking read\n");

        char buffer[64];
        memset(buffer, 0, sizeof(buffer));
        long t0 = sys_uptime_ms();
        long n = read(cli, buffer, sizeof(buffer));
        long waited = sys_uptime_ms() - t0;
        check(n == 4 && memcmp(buffer, "late", 4) == 0, "a blocking read did not return the peer's message");
        check(waited >= 100, "a blocking read returned before the peer had written");
        printf("tcptest: posix: the blocking read returned after %ld ms\n", waited);

        check(fcntl(cli, F_SETFL, O_NONBLOCK) == 0 && (fcntl(cli, F_GETFL) & O_NONBLOCK),
              "O_NONBLOCK could not be set on a socket");
        errno = 0;
        check(read(cli, buffer, sizeof(buffer)) == -1 && errno == EAGAIN,
              "a non-blocking read with nothing pending was not EAGAIN");
        errno = 0;
        check(recv(cli, buffer, sizeof(buffer), MSG_DONTWAIT) == -1 && errno == EAGAIN,
              "recv(MSG_DONTWAIT) with nothing pending was not EAGAIN");
        check(fcntl(cli, F_SETFL, 0) == 0, "O_NONBLOCK could not be cleared");

        static char big[POSIX_BIG];
        for (int i = 0; i < POSIX_BIG; i++) {
            big[i] = (char)('A' + (i * 13 + i / 97) % 26);
        }
        printf("tcptest: posix: O_NONBLOCK answered EAGAIN, writing 16 KiB\n");
        long put = write(cli, big, POSIX_BIG);
        check(put == POSIX_BIG, "one write() did not return every byte it was given");
        printf("tcptest: posix: write returned %ld, waiting for the peer's confirmation\n", put);
        memset(buffer, 0, sizeof(buffer));
        n = read(cli, buffer, sizeof(buffer));
        check(n == 4 && memcmp(buffer, "done", 4) == 0, "the peer did not confirm the transfer arrived intact");

        void *peer_result = (void *)1;
        check(pthread_join(peer, &peer_result) == 0 && peer_result == (void *)0,
              "the peer thread saw a byte out of place");
        close(srv);
        n = read(cli, buffer, sizeof(buffer));
        check(n == 0, "read() after the server closed was not 0");
        printf("tcptest: posix: end of stream seen\n");
        close(cli);
        close(lis);
    }

    int dgram = (int)sys_socket(OS_SOCKET_DGRAM);
    check(sys_listen(dgram) < 0, "listen on a datagram socket succeeded");
    check(sys_send(dgram, "x", 1) < 0, "send on a datagram socket succeeded");
    check(sys_receive(dgram, buffer, 1) < 0, "recv on a datagram socket succeeded");
    errno = 0;
    check(recv(dgram, buffer, 1, MSG_PEEK) == -1 && errno == EOPNOTSUPP,
          "a peek at a datagram socket was not refused - the message queue "
          "is not peekable and a silent recvfrom is what MSG_PEEK exists to "
          "stop (M183)");
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
