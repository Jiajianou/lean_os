/* user_space/bin/netrecv.c - M116: a stream from another machine, byte for byte.
 *
 *   netrecv A.B.C.D PORT BYTES
 *
 * Connects, reads until end of stream, and checks that exactly BYTES
 * arrived and that every one of them is the pattern below. Prints what
 * it measured; exits 0 only if all of it was right.
 *
 * Every network test before this one talked to this machine over
 * loopback or exchanged a few hundred bytes with a DNS server, and so
 * none of them ever sent a full-sized segment through the NIC's receive
 * ring - which is where M116 found one frame in five being corrupted,
 * silently, since M27. The boot self-test runs this against a stream the
 * HOST produces (tools/qemu-serial-test.sh gives QEMU a `guestfwd` that
 * runs `cat` on a file it wrote with the same pattern), so the bytes
 * come through QEMU's SLIRP, the RTL8139 and the whole of kernel/net the
 * way a web page does, and are checked against something this program
 * did not send.
 *
 * Through the POSIX socket calls, because that is what NetSurf's libcurl
 * uses and so that is the path worth grading.
 *
 * The pattern is written in one other place, tools/qemu-serial-test.sh,
 * and the two must agree: byte i is (i * 7 + (i >> 9)) & 0xFF. The
 * second term makes a byte's value depend on where in the stream it is
 * beyond the first 512, so a segment delivered twice or at the wrong
 * offset cannot match by coincidence. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static unsigned char pattern(unsigned long i) {
    return (unsigned char)((i * 7u + (i >> 9)) & 0xFFu);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        printf("usage: netrecv A.B.C.D PORT BYTES\n");
        return 2;
    }
    unsigned long expect = strtoul(argv[3], NULL, 10);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        printf("netrecv: '%s' is not an IPv4 address\n", argv[1]);
        return 2;
    }

    long started = now_ms();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("netrecv: could not connect to %s:%s\n", argv[1], argv[2]);
        return 1;
    }
    long connected = now_ms();

    static unsigned char buf[8192];
    unsigned long got = 0, wrong = 0, first_wrong = 0;
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            printf("netrecv: read failed after %lu bytes\n", got);
            close(fd);
            return 1;
        }
        if (n == 0) {
            break; /* end of stream - the peer's FIN */
        }
        for (long k = 0; k < n; k++) {
            if (buf[k] != pattern(got + (unsigned long)k)) {
                if (!wrong) {
                    first_wrong = got + (unsigned long)k;
                }
                wrong++;
            }
        }
        got += (unsigned long)n;
    }
    long finished = now_ms();
    close(fd);

    long ms = finished - connected;
    printf("netrecv: %lu bytes in %ld ms after a %ld ms connect (%lu KiB/s), %lu wrong\n",
           got, ms, connected - started,
           ms > 0 ? (got / 1024ul) * 1000ul / (unsigned long)ms : 0ul, wrong);
    if (wrong) {
        printf("netrecv: the first wrong byte is at offset %lu\n", first_wrong);
        return 1;
    }
    if (got != expect) {
        printf("netrecv: expected %lu bytes\n", expect);
        return 1;
    }
    return 0;
}
