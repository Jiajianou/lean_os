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

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
        printf("netrecv: '%s' is not an IPv4 address\n", argv[1]);
        return 2;
    }

    long started = now_ms();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        printf("netrecv: could not connect to %s:%s\n", argv[1], argv[2]);
        return 1;
    }
    long connected = now_ms();

    static unsigned char buffer[8192];
    unsigned long got = 0, wrong = 0, first_wrong = 0;
    for (;;) {
        long n = read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            printf("netrecv: read failed after %lu bytes\n", got);
            close(fd);
            return 1;
        }
        if (n == 0) {
            break;
        }
        for (long k = 0; k < n; k++) {
            if (buffer[k] != pattern(got + (unsigned long)k)) {
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
