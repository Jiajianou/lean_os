#include <stdio.h>

#include "dns.h"
#include "os_net.h"
#include "paths.h"
#include "syscall_wrappers.h"

static void print_ip(uint32_t ip) {
    printf("%d.%d.%d.%d", (int)((ip >> 24) & 0xFF), (int)((ip >> 16) & 0xFF),
            (int)((ip >> 8) & 0xFF), (int)(ip & 0xFF));
}

static int selftest(void) {
    int failures = 0;
    uint32_t ip = 0;

    static const uint8_t compressed[] = {
        0xAB, 0xCD, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        3, 'w','w','w', 7, 'e','x','a','m','p','l','e', 3, 'c','o','m', 0,
        0x00, 0x01, 0x00, 0x01,
        0xC0, 0x0C,
        0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04, 93, 184, 216, 34,
    };
    if (dns_parse_response(compressed, (int)sizeof(compressed), 0xABCD,
                            "www.example.com", &ip) != 0 || ip != 0x5DB8D822u) {
        printf("nslookup: FAILED - a compression pointer was not followed\n");
        failures++;
    }

    static const uint8_t cname[] = {
        0x00, 0x2A, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        1, 'a', 4, 't','e','s','t', 0,
        0x00, 0x01, 0x00, 0x01,
        0xC0, 0x0C, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04, 1, 'b', 0xC0, 0x0E,
        1, 'b', 4, 't','e','s','t', 0,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04, 10, 0, 0, 7,
    };
    if (dns_parse_response(cname, (int)sizeof(cname), 0x002A, "a.test", &ip) != 0 ||
        ip != 0x0A000007u) {
        printf("nslookup: FAILED - a CNAME was not followed to its A record\n");
        failures++;
    }

    static const uint8_t loop[] = {
        0x00, 0x2B, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x0C,
        0x00, 0x01, 0x00, 0x01,
    };
    if (dns_parse_response(loop, (int)sizeof(loop), 0x002B, 0, &ip) != -4) {
        printf("nslookup: FAILED - a self-referential compression pointer was not refused\n");
        failures++;
    }

    if (dns_parse_response(compressed, (int)sizeof(compressed), 0x0001,
                            "www.example.com", &ip) != -4) {
        printf("nslookup: FAILED - a reply with the wrong id was accepted\n");
        failures++;
    }

    if (dns_parse_response(compressed, (int)sizeof(compressed), 0xABCD,
                            "elsewhere.test", &ip) != -4) {
        printf("nslookup: FAILED - a reply for a different name was accepted\n");
        failures++;
    }

    static const uint8_t nxdomain[] = {
        0x00, 0x2C, 0x81, 0x83, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        2, 'n','x', 4, 't','e','s','t', 0, 0x00, 0x01, 0x00, 0x01,
    };
    if (dns_parse_response(nxdomain, (int)sizeof(nxdomain), 0x002C, "nx.test", &ip) != -3) {
        printf("nslookup: FAILED - NXDOMAIN was not reported as a definite no\n");
        failures++;
    }

    if (dns_parse_response(compressed, 20, 0xABCD, "www.example.com", &ip) != -4) {
        printf("nslookup: FAILED - a truncated reply was not refused\n");
        failures++;
    }

    uint8_t q[64];
    int qn = dns_build_query("a.test", 0x1234, q, sizeof(q));
    if (qn != 12 + 8 + 4 || q[0] != 0x12 || q[1] != 0x34 || q[12] != 1 || q[13] != 'a') {
        printf("nslookup: FAILED - the query encoder produced the wrong bytes\n");
        failures++;
    }

    if (failures == 0) {
        printf("nslookup: all parser checks passed\n");
    }
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 's') {
        return selftest();
    }
    if (argc < 2 || !argv[1][0]) {
        printf("usage: nslookup NAME  |  nslookup -s   (parser self-test)\n");
        return 2;
    }

    os_netconf_t conf;
    if (sys_netconf(&conf) != 0) {
        printf("nslookup: no network on this machine\n");
        return 1;
    }
    uint32_t servers[DNS_MAX_SERVERS];
    int nservers = dns_servers(servers, DNS_MAX_SERVERS);
    if (nservers == 0) {
        printf("nslookup: no nameserver configured - DHCP handed none over "
               "and " PATH_RESOLV_CONF " lists none\n");
        return 1;
    }
    printf(nservers == 1 ? "server:  " : "servers: ");
    for (int i = 0; i < nservers; i++) {
        if (i) {
            printf(", ");
        }
        print_ip(servers[i]);
    }
    printf("\n");

    uint32_t ip = 0;
    int rc = dns_resolve(argv[1], &ip);
    switch (rc) {
    case 0:
        printf("%s has address ", argv[1]);
        print_ip(ip);
        printf("\n");
        return 0;
    case -2:
        printf("nslookup: %s: no reply from the server (timed out)\n", argv[1]);
        return 1;
    case -3:
        printf("nslookup: %s: the server says there is no such name\n", argv[1]);
        return 1;
    case -4:
        printf("nslookup: %s: the reply was malformed or answered a different question\n", argv[1]);
        return 1;
    default:
        printf("nslookup: %s: could not ask (no socket, or no DNS server configured)\n", argv[1]);
        return 1;
    }
}
