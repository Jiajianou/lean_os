#include <stdio.h>
#include <string.h>

#include "os_network.h"
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

    int ls = (int)sys_socket(OS_SOCKET_STREAM);
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

    char request[512];
    long rdeadline = sys_uptime_ms() + 3000;
    while (sys_uptime_ms() < rdeadline) {
        long n = sys_receive(cs, request, sizeof(request));
        if (n > 0) {
            break;
        }
        if (n < 0) {
            break;
        }
        sys_yield();
    }

    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 35\r\n"
        "Connection: close\r\n"
        "\r\n"
        BODY;
    long sent = 0;
    long slen = (long)sizeof(response) - 1;
    long sdeadline = sys_uptime_ms() + 5000;
    while (sent < slen && sys_uptime_ms() < sdeadline) {
        long n = sys_send(cs, response + sent, (uint32_t)(slen - sent));
        if (n < 0) {
            break;
        }
        if (n == 0) {
            sys_yield();
            continue;
        }
        sent += n;
    }
    long drain = sys_uptime_ms() + 500;
    while (sys_uptime_ms() < drain) {
        sys_yield();
    }
    sys_close(cs);
    sys_close(ls);
    printf("httpd: served %ld byte(s)\n", sent);
    return sent == slen ? 0 : 1;
}
