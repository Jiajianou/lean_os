/* user_space/bin/nettest.c
 *
 * M64's self-test, in user space, for the same reason M52's badptr.c and
 * M63's libctest.c are: the thing being tested is the *syscall* surface,
 * and a test that called the kernel's own socket functions from
 * kernel_main would prove the layer underneath the one that is new.
 * Every claim here goes through int 0x80 exactly as a real program's
 * would. The kernel self-test spawns this and grades its exit code.
 *
 * Two of these assertions are about the network working and five are
 * about it failing correctly, which is the right ratio for a subsystem
 * whose entire new surface is reachable from user space for the first
 * time. M52's rule - no user-triggerable panic - had never been applied
 * to kernel/net, because until this milestone user space could not reach
 * it. Sending to an address with no route used to be a panic().
 *
 * Exit code 0 for all-passed, 1 for a failure, and the failure prints
 * which one - the pattern every one of these test programs uses.
 */
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

/* The loopback address. Everything in 127/8 is us (kernel/net/net.h), so
 * a datagram sent here never touches the NIC - which is what makes the
 * socket layer testable on a machine with no network at all, and is why
 * the substance of this test does not depend on QEMU's SLIRP answering
 * anything. */
#define LOOPBACK OS_IPV4(127, 0, 0, 1)
#define TEST_PORT 7777

int main(void) {
    /* ---- the configuration exists and is coherent --------------------- */
    os_netconf_t conf;
    if (sys_netconf(&conf) != 0) {
        printf("nettest: no NIC - nothing to test\n");
        return 0; /* the same "installed but untested this boot" degradation the rest of this OS uses */
    }
    check(conf.ip != 0, "the machine has no address at all");
    check(conf.mask != 0, "the machine has no netmask");
    check((conf.gateway & conf.mask) == (conf.ip & conf.mask),
          "the gateway is not on the same subnet as this machine");

    /* ---- a real round trip through UDP, IP and the socket layer -------- */
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

    /* ...and the queue is empty again afterwards, which is the half of a
     * ring buffer that a single-datagram test never exercises. */
    check(sys_sockpoll(server) == 0, "the socket still reports a datagram after reading it");
    check(sys_recvfrom(server, got, sizeof(got), &from) < 0,
          "sys_recvfrom on an empty socket did not fail");

    /* ---- truncation, which UDP is allowed to do and must admit to ------ */
    sys_sendto(client, LOOPBACK, TEST_PORT, payload, (uint32_t)strlen(payload));
    char small[8];
    long trunc = sys_recvfrom(server, small, sizeof(small), 0);
    check(trunc == (long)sizeof(small), "a truncated recvfrom did not report the length it kept");
    check(sys_sockpoll(server) == 0, "the rest of a truncated datagram was left queued");

    /* ---- and now the ways it is supposed to fail ----------------------- */
    int taken = (int)sys_socket(OS_SOCK_DGRAM);
    check(sys_bind(taken, TEST_PORT) < 0, "two sockets bound the same port");
    check(sys_bind(client, 9999) < 0, "an already-bound socket was bound a second time");
    sys_close(taken);

    check(sys_sendto(999, LOOPBACK, TEST_PORT, payload, 4) < 0,
          "sendto on a descriptor that is not a socket succeeded");
    check(sys_recvfrom(1, got, sizeof(got), 0) < 0,
          "recvfrom on stdout succeeded");
    check(sys_sockpoll(0) < 0, "sockpoll on stdin succeeded");

    /* An address on our subnet that nothing answers ARP for. This one
     * check found two ways to halt the machine, both of them written
     * long before anything in user space could reach this code:
     * `ip_send` *panicked* on an ARP timeout, and the bounded wait it
     * panicked after was a `hlt` loop - which, inside a syscall taken
     * through an interrupt gate, halts the CPU with interrupts off and
     * never returns. The first version of this test hung the machine
     * mid-boot with no panic and no output. Both are fixed in ip.c, and
     * both were invisible for thirty-six milestones because the only
     * caller was a self-test pinging a gateway arranged to answer. */
    uint32_t nowhere = (conf.ip & conf.mask) | 0xFE;
    /* M116: by the THIRD send, not the first. The first two are held for
     * ARP's answer rather than dropped (kernel/net/arp.c, arp_hold) -
     * dropping them cost every first contact with a machine on this link
     * a whole TCP retransmission timeout - and a neighbour that has been
     * asked three times and never answered is unreachable, and says so.
     * Linux behaves the same way: a first datagram to an unresolved
     * neighbour is accepted, and EHOSTUNREACH comes once resolution has
     * visibly failed. What this check has always been for is unchanged:
     * no hang, no panic, and a program that is told. */
    sys_sendto(client, nowhere, TEST_PORT, payload, 4);
    sys_sendto(client, nowhere, TEST_PORT, payload, 4);
    check(sys_sendto(client, nowhere, TEST_PORT, payload, 4) < 0,
          "sendto to a neighbour that never answered ARP did not fail by the third try");

    /* Oversize. UDP_MAX_PAYLOAD is 1472; this is comfortably past it and
     * must be refused rather than truncated, because a datagram is a
     * message and half a message is not a shorter one. */
    static char huge[4096];
    memset(huge, 'x', sizeof(huge));
    check(sys_sendto(client, LOOPBACK, TEST_PORT, huge, sizeof(huge)) < 0,
          "an oversized datagram was accepted");

    sys_close(server);
    sys_close(client);

    /* ---- closing a socket gives the slot back -------------------------- */
    /* MAX_SOCKETS is 32. Opening and closing more than that in a loop is
     * the M50 lesson - a table entry that is allocated and never freed
     * looks fine until the thirty-third caller - and it is cheap enough
     * to just do rather than reason about. */
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
