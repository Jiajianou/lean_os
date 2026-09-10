/* tests/fakes/fake_user_net.c - M114
 *
 * A network for user_space/lib/dns.c to resolve names on, with no
 * network in it.
 *
 * ---- why this fake and not a real socket ------------------------------
 *
 * M114 exists because of a nameserver that accepted every query and
 * answered none. That is the failure this resolver now has to survive,
 * and it is precisely the one that cannot be reproduced against a real
 * server: you cannot ask a working resolver to please start
 * black-holing, and a test that points at an address nothing listens on
 * grades ICMP-unreachable rather than silence. So the server behaviour
 * is the thing this fake makes settable, one per address:
 *
 *   FAKE_DNS_SILENT    accepts the datagram, answers nothing - the bug
 *   FAKE_DNS_ANSWER    a well-formed A record
 *   FAKE_DNS_NXDOMAIN  a definite "no such name"
 *   FAKE_DNS_GARBAGE   a reply that is not a DNS message at all
 *
 * Time is a counter this file advances rather than a clock, so a three
 * second timeout costs no seconds: sys_uptime_ms moves forward on every
 * poll, which is what makes "all servers silent" a test that finishes.
 */
#include "fakes.h"

#include <string.h>

#include "os_net.h"

/* The declarations these stand in for, so the compiler checks this fake
 * against them - fake_socket.c's header comment explains what happens
 * when it cannot, and it happened there. */
#include "syscall_wrappers.h"

#define MAX_SERVERS 8
#define MAX_MSG 512

typedef struct {
    uint32_t ip;
    int behaviour;
    uint32_t answer;      /* host order, for FAKE_DNS_ANSWER */
    int queries_seen;
} server_t;

static server_t servers[MAX_SERVERS];
static int server_count;

static uint32_t conf_dns;
static int conf_ok = 1;

static const char *resolv_text;
static long resolv_len;
static int resolv_present;

static long fake_now_ms;
static int socket_open;
static int bind_ok = 1;
static int socket_ok = 1;

/* One pending reply at a time. The resolver polls, so a reply produced
 * when the query was sent is read on the next poll - which is the same
 * ordering a real socket gives it. */
static struct {
    int pending;
    uint32_t from_ip;
    uint8_t data[MAX_MSG];
    long len;
} inbox;

void fake_user_net_reset(void) {
    memset(servers, 0, sizeof(servers));
    server_count = 0;
    conf_dns = 0;
    conf_ok = 1;
    resolv_text = 0;
    resolv_len = 0;
    resolv_present = 0;
    fake_now_ms = 1000;
    socket_open = 0;
    bind_ok = 1;
    socket_ok = 1;
    memset(&inbox, 0, sizeof(inbox));
}

void fake_user_net_add_server(uint32_t ip, int behaviour, uint32_t answer) {
    if (server_count < MAX_SERVERS) {
        servers[server_count].ip = ip;
        servers[server_count].behaviour = behaviour;
        servers[server_count].answer = answer;
        servers[server_count].queries_seen = 0;
        server_count++;
    }
}

void fake_user_net_set_dhcp_dns(uint32_t ip) { conf_dns = ip; }
void fake_user_net_set_netconf_fails(int fails) { conf_ok = !fails; }

void fake_user_net_set_resolv_conf(const char *text, long len) {
    resolv_text = text;
    resolv_len = len;
    resolv_present = 1;
}

void fake_user_net_no_resolv_conf(void) {
    resolv_text = 0;
    resolv_len = 0;
    resolv_present = 0;
}

void fake_user_net_set_socket_fails(int fails) { socket_ok = !fails; }
void fake_user_net_set_bind_fails(int fails) { bind_ok = !fails; }

int fake_user_net_queries_seen(uint32_t ip) {
    for (int i = 0; i < server_count; i++) {
        if (servers[i].ip == ip) {
            return servers[i].queries_seen;
        }
    }
    return -1;
}

/* ---- the syscalls ----------------------------------------------------- */

long sys_uptime_ms(void) { return fake_now_ms; }
long sys_getpid(void) { return 42; }
/* sys_yield is fake_user_syscalls.c's - one definition, because two
 * fakes that both define it is a link error and two that disagree would
 * be worse. */

long sys_netconf(os_netconf_t *out) {
    if (!conf_ok || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->dns = conf_dns;
    out->ip = 0x0A000210u;
    out->gateway = 0x0A000202u;
    out->leased = 1;
    return 0;
}

long sys_readfile(const char *name, void *buf, size_t maxlen) {
    (void)name;
    if (!resolv_present) {
        return -1;
    }
    /* The real one's contract, which is the whole reason this fake
     * bothers: it returns the FILE's size and copies at most maxlen. A
     * fake that returned min(size, maxlen) would make the overread that
     * shipped in M114's first version untestable, because the number it
     * returned would always have been safe. */
    size_t n = (size_t)resolv_len < maxlen ? (size_t)resolv_len : maxlen;
    if (buf && n) {
        memcpy(buf, resolv_text, n);
    }
    return resolv_len;
}

long sys_socket(int type) {
    (void)type;
    if (!socket_ok) {
        return -1;
    }
    socket_open = 1;
    return 3;
}

long sys_bind(int fd, uint16_t port) {
    (void)fd;
    (void)port;
    return bind_ok ? 49152 : -1;
}

long sys_close(int fd) {
    (void)fd;
    socket_open = 0;
    return 0;
}

/* Builds the reply this server would send, if it sends one. The message
 * is assembled here rather than by calling dns_build_query's opposite
 * number, so a bug shared between the builder and the parser cannot hide
 * - the same argument tests/test_ospkg.c makes about writing archives by
 * hand. */
static void answer_for(server_t *s, const uint8_t *query, long qlen) {
    if (s->behaviour == FAKE_DNS_SILENT) {
        return;
    }
    if (inbox.pending) {
        return; /* one at a time; the resolver drains before the next round */
    }
    if (s->behaviour == FAKE_DNS_GARBAGE) {
        inbox.pending = 1;
        inbox.from_ip = s->ip;
        inbox.len = 4;
        memcpy(inbox.data, "\xde\xad\xbe\xef", 4);
        return;
    }
    if (qlen < 12 || qlen > MAX_MSG) {
        return;
    }
    uint8_t *r = inbox.data;
    memcpy(r, query, (size_t)qlen);       /* id, then the question, echoed */
    r[2] = 0x81;                          /* response, recursion available */
    r[3] = (uint8_t)(s->behaviour == FAKE_DNS_NXDOMAIN ? 0x83 : 0x80);
    r[4] = 0; r[5] = 1;                   /* one question */
    long len = qlen;
    if (s->behaviour == FAKE_DNS_NXDOMAIN) {
        r[6] = 0; r[7] = 0;               /* no answers */
    } else {
        r[6] = 0; r[7] = 1;               /* one answer */
        r[len++] = 0xC0; r[len++] = 0x0C; /* a pointer back at the question */
        r[len++] = 0x00; r[len++] = 0x01; /* type A */
        r[len++] = 0x00; r[len++] = 0x01; /* class IN */
        r[len++] = 0x00; r[len++] = 0x00;
        r[len++] = 0x00; r[len++] = 0x3C; /* ttl 60 */
        r[len++] = 0x00; r[len++] = 0x04; /* rdlength 4 */
        r[len++] = (uint8_t)(s->answer >> 24);
        r[len++] = (uint8_t)(s->answer >> 16);
        r[len++] = (uint8_t)(s->answer >> 8);
        r[len++] = (uint8_t)(s->answer);
    }
    r[8] = 0; r[9] = 0; r[10] = 0; r[11] = 0; /* no ns, no additional */
    inbox.pending = 1;
    inbox.from_ip = s->ip;
    inbox.len = len;
}

long sys_sendto(int fd, uint32_t ip, uint16_t port, const void *data,
                uint32_t len) {
    (void)fd;
    (void)port;
    for (int i = 0; i < server_count; i++) {
        if (servers[i].ip == ip) {
            servers[i].queries_seen++;
            answer_for(&servers[i], (const uint8_t *)data, (long)len);
            return (long)len;
        }
    }
    return (long)len; /* sent into a void nobody is listening on */
}

long sys_sockpoll(int fd) {
    (void)fd;
    /* Time only moves when the resolver looks at the clock, which is
     * what makes a three-second timeout cost nothing here. 100 ms a
     * poll, so a one-second retransmit cadence takes ten. */
    fake_now_ms += 100;
    return inbox.pending ? 1 : 0;
}

long sys_recvfrom(int fd, void *data, uint32_t max, os_sockaddr_t *from) {
    (void)fd;
    if (!inbox.pending) {
        return -1;
    }
    long n = inbox.len < (long)max ? inbox.len : (long)max;
    if (data && n > 0) {
        memcpy(data, inbox.data, (size_t)n);
    }
    if (from) {
        from->ip = inbox.from_ip;
        from->port = 53;
    }
    inbox.pending = 0;
    return n;
}
