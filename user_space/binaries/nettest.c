#include <stdio.h>
#include <string.h>

#include "os_net.h"
#include "syscall_wrappers.h"

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("nettest: FAILED - %s\n", what);
        failures++;
    }
}

#define LOOPBACK OS_IPV4(127, 0, 0, 1)
#define TEST_PORT 7777

int main(void) {
    os_netconf_t conf;
    if (sys_netconf(&conf) != 0) {
        printf("nettest: no NIC - nothing to test\n");
        return 0;
    }
    check(conf.ip != 0, "the machine has no address at all");
    check(conf.mask != 0, "the machine has no netmask");
    check((conf.gateway & conf.mask) == (conf.ip & conf.mask),
          "the gateway is not on the same subnet as this machine");

    int server = (int)sys_socket(OS_SOCK_DGRAM);
    check(server >= 0, "sys_socket returned no descriptor");
    check(sys_bind(server, TEST_PORT) == TEST_PORT, "sys_bind did not return the port it bound");

    int client = (int)sys_socket(OS_SOCK_DGRAM);
    check(client >= 0, "a second sys_socket returned no descriptor");
    long ephemeral = sys_bind(client, 0);
    check(ephemeral >= 49152 && ephemeral <= 65535,
          "sys_bind(0) did not return a port from the ephemeral range");

    const char *payload = "the network is reachable";
    long sent = sys_sendto(client, LOOPBACK, TEST_PORT, payload, (uint32_t)strlen(payload));
    check(sent == (long)strlen(payload), "sys_sendto did not send the whole payload");

    check(sys_sockpoll(server) == 1, "the datagram did not arrive on the bound socket");

    char got[64];
    os_sockaddr_t from;
    memset(got, 0, sizeof(got));
    long n = sys_recvfrom(server, got, sizeof(got), &from);
    check(n == (long)strlen(payload), "sys_recvfrom returned the wrong length");
    check(memcmp(got, payload, strlen(payload)) == 0, "the datagram's contents changed in transit");
    check(from.ip == conf.ip, "recvfrom reported the wrong source address");
    check(from.port == (uint16_t)ephemeral, "recvfrom reported the wrong source port");

    check(sys_sockpoll(server) == 0, "the socket still reports a datagram after reading it");
    check(sys_recvfrom(server, got, sizeof(got), &from) < 0,
          "sys_recvfrom on an empty socket did not fail");

    sys_sendto(client, LOOPBACK, TEST_PORT, payload, (uint32_t)strlen(payload));
    char small[8];
    long trunc = sys_recvfrom(server, small, sizeof(small), 0);
    check(trunc == (long)sizeof(small), "a truncated recvfrom did not report the length it kept");
    check(sys_sockpoll(server) == 0, "the rest of a truncated datagram was left queued");

    int taken = (int)sys_socket(OS_SOCK_DGRAM);
    check(sys_bind(taken, TEST_PORT) < 0, "two sockets bound the same port");
    check(sys_bind(client, 9999) < 0, "an already-bound socket was bound a second time");
    sys_close(taken);

    check(sys_sendto(999, LOOPBACK, TEST_PORT, payload, 4) < 0,
          "sendto on a descriptor that is not a socket succeeded");
    check(sys_recvfrom(1, got, sizeof(got), 0) < 0,
          "recvfrom on stdout succeeded");
    check(sys_sockpoll(0) < 0, "sockpoll on stdin succeeded");

    uint32_t nowhere = (conf.ip & conf.mask) | 0xFE;
    sys_sendto(client, nowhere, TEST_PORT, payload, 4);
    sys_sendto(client, nowhere, TEST_PORT, payload, 4);
    check(sys_sendto(client, nowhere, TEST_PORT, payload, 4) < 0,
          "sendto to a neighbour that never answered ARP did not fail by the third try");

    static char huge[4096];
    memset(huge, 'x', sizeof(huge));
    check(sys_sendto(client, LOOPBACK, TEST_PORT, huge, sizeof(huge)) < 0,
          "an oversized datagram was accepted");

    sys_close(server);
    sys_close(client);

    for (int i = 0; i < 100; i++) {
        int fd = (int)sys_socket(OS_SOCK_DGRAM);
        if (fd < 0) {
            check(0, "the socket table ran out - a closed socket is not being freed");
            break;
        }
        sys_bind(fd, 0);
        sys_close(fd);
    }

    if (failures) {
        printf("nettest: %d check(s) failed\n", failures);
        return 1;
    }
    printf("nettest: all checks passed\n");
    return 0;
}
