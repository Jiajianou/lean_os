/* user_space/bin/httpd.c
 *
 * M73's test server, and it exists so that the self-test needs no host
 * network at all.
 *
 * Same reasoning as M66's loopback TCP test: the point is to check *this*
 * machine's client against a server whose every byte is known, not to
 * check whether the machine running QEMU happens to have internet. It
 * serves exactly what it is told to and exits, so a test can assert on
 * bytes rather than on the weather.
 *
 *   httpd PORT       serve one request, then exit
 *
 * Deliberately serves ONE request and stops. A test wants a server that
 * is finished when the test is, and a loop would be a process the test
 * then has to remember to kill.
 */
#include <stdio.h>
#include <string.h>

#include "os_net.h"
#include "syscall_wrappers.h"

#define BODY "lean_os fetched this over loopback\n"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: httpd PORT\n");
        return 2;
    }
    uint16_t port = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
        port = (uint16_t)(port * 10 + (*p - '0'));
    }
    if (port == 0) {
        return 2;
    }

    int ls = (int)sys_socket(OS_SOCK_STREAM);
    if (ls < 0 || sys_bind(ls, port) <= 0 || sys_listen(ls) != 0) {
        printf("httpd: cannot listen on %d\n", (int)port);
        return 1;
    }
    printf("httpd: listening on %d\n", (int)port);

    long deadline = sys_uptime_ms() + 15000;
    int cs = -1;
    while (sys_uptime_ms() < deadline && cs < 0) {
        os_sockaddr_t from;
        cs = (int)sys_accept(ls, &from);
        if (cs < 0) {
            sys_yield();
        }
    }
    if (cs < 0) {
        printf("httpd: nobody connected\n");
        return 1;
    }

    /* Drain the request. Not parsed - this server has exactly one answer
     * and giving it regardless is honest for a fixture; pretending to
     * route would be a second thing to get wrong. */
    char req[512];
    long rdeadline = sys_uptime_ms() + 3000;
    while (sys_uptime_ms() < rdeadline) {
        long n = sys_recv(cs, req, sizeof(req));
        if (n > 0) {
            break;
        }
        if (n < 0) {
            break;
        }
        sys_yield();
    }

    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 35\r\n"
        "Connection: close\r\n"
        "\r\n"
        BODY;
    long sent = 0;
    long slen = (long)sizeof(resp) - 1;
    long sdeadline = sys_uptime_ms() + 5000;
    while (sent < slen && sys_uptime_ms() < sdeadline) {
        long n = sys_send(cs, resp + sent, (uint32_t)(slen - sent));
        if (n < 0) {
            break;
        }
        if (n == 0) {
            sys_yield();
            continue;
        }
        sent += n;
    }
    /* Let the bytes drain before the close turns into a FIN. */
    long drain = sys_uptime_ms() + 500;
    while (sys_uptime_ms() < drain) {
        sys_yield();
    }
    sys_close(cs);
    sys_close(ls);
    printf("httpd: served %ld byte(s)\n", sent);
    return sent == slen ? 0 : 1;
}
