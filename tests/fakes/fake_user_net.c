#include "fakes.h"

#include <string.h>

#include "os_net.h"

#include "syscall_wrappers.h"

#define MAX_SERVERS 8
#define MAX_MESSAGE 512

typedef struct {
    uint32_t ip;
    int behaviour;
    uint32_t answer;
    int queries_seen;
} server_t;

static server_t servers[MAX_SERVERS];
static int server_count;

static uint32_t conf_dns;
static int conf_ok = 1;

static const char *resolv_text;
static long resolv_length;
static int resolv_present;

static long fake_now_ms;
static int socket_open;
static int bind_ok = 1;
static int socket_ok = 1;

static struct {
    int pending;
    uint32_t from_ip;
    uint8_t data[MAX_MESSAGE];
    long length;
} inbox;

void fake_user_net_reset(void) {
    memset(servers, 0, sizeof(servers));
    server_count = 0;
    conf_dns = 0;
    conf_ok = 1;
    resolv_text = 0;
    resolv_length = 0;
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

void fake_user_net_set_resolv_conf(const char *text, long length) {
    resolv_text = text;
    resolv_length = length;
    resolv_present = 1;
}

void fake_user_net_no_resolv_conf(void) {
    resolv_text = 0;
    resolv_length = 0;
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

long sys_uptime_ms(void) { return fake_now_ms; }
long sys_getpid(void) { return 42; }

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

long sys_readfile(const char *name, void *buffer, size_t maxlen) {
    (void)name;
    if (!resolv_present) {
        return -1;
    }
    size_t n = (size_t)resolv_length < maxlen ? (size_t)resolv_length : maxlen;
    if (buffer && n) {
        memcpy(buffer, resolv_text, n);
    }
    return resolv_length;
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

static void answer_for(server_t *s, const uint8_t *query, long qlen) {
    if (s->behaviour == FAKE_DNS_SILENT) {
        return;
    }
    if (inbox.pending) {
        return;
    }
    if (s->behaviour == FAKE_DNS_GARBAGE) {
        inbox.pending = 1;
        inbox.from_ip = s->ip;
        inbox.length = 4;
        memcpy(inbox.data, "\xde\xad\xbe\xef", 4);
        return;
    }
    if (qlen < 12 || qlen > MAX_MESSAGE) {
        return;
    }
    uint8_t *r = inbox.data;
    memcpy(r, query, (size_t)qlen);
    r[2] = 0x81;
    r[3] = (uint8_t)(s->behaviour == FAKE_DNS_NXDOMAIN ? 0x83 : 0x80);
    r[4] = 0; r[5] = 1;
    long length = qlen;
    if (s->behaviour == FAKE_DNS_NXDOMAIN) {
        r[6] = 0; r[7] = 0;
    } else {
        r[6] = 0; r[7] = 1;
        r[length++] = 0xC0; r[length++] = 0x0C;
        r[length++] = 0x00; r[length++] = 0x01;
        r[length++] = 0x00; r[length++] = 0x01;
        r[length++] = 0x00; r[length++] = 0x00;
        r[length++] = 0x00; r[length++] = 0x3C;
        r[length++] = 0x00; r[length++] = 0x04;
        r[length++] = (uint8_t)(s->answer >> 24);
        r[length++] = (uint8_t)(s->answer >> 16);
        r[length++] = (uint8_t)(s->answer >> 8);
        r[length++] = (uint8_t)(s->answer);
    }
    r[8] = 0; r[9] = 0; r[10] = 0; r[11] = 0;
    inbox.pending = 1;
    inbox.from_ip = s->ip;
    inbox.length = length;
}

long sys_sendto(int fd, uint32_t ip, uint16_t port, const void *data,
                uint32_t length) {
    (void)fd;
    (void)port;
    for (int i = 0; i < server_count; i++) {
        if (servers[i].ip == ip) {
            servers[i].queries_seen++;
            answer_for(&servers[i], (const uint8_t *)data, (long)length);
            return (long)length;
        }
    }
    return (long)length;
}

long sys_sockpoll(int fd) {
    (void)fd;
    fake_now_ms += 100;
    return inbox.pending ? 1 : 0;
}

long sys_recvfrom(int fd, void *data, uint32_t max, os_sockaddr_t *from) {
    (void)fd;
    if (!inbox.pending) {
        return -1;
    }
    long n = inbox.length < (long)max ? inbox.length : (long)max;
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
