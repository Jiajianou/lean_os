#include "dns.h"

#include <string.h>

#include "os_net.h"
#include "paths.h"
#include "syscall_wrappers.h"

#define DNS_PORT        53
#define DNS_TIMEOUT_MS  3000
#define DNS_MAX_MSG     512
#define DNS_MAX_CNAME   4

#define DNS_TYPE_A      1
#define DNS_TYPE_CNAME  5
#define DNS_CLASS_IN    1

static int encode_name(const char *name, uint8_t *out, int cap) {
    int n = 0;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') {
            dot++;
        }
        int len = (int)(dot - p);
        if (len == 0 || len > 63 || n + len + 1 >= cap) {
            return -1;
        }
        out[n++] = (uint8_t)len;
        for (int i = 0; i < len; i++) {
            out[n++] = (uint8_t)p[i];
        }
        p = *dot ? dot + 1 : dot;
    }
    if (n + 1 > cap) {
        return -1;
    }
    out[n++] = 0;
    return n;
}

static int decode_name(const uint8_t *msg, int len, int pos, char *out, int out_cap) {
    int consumed = 0;
    int jumps = 0;
    int written = 0;
    int followed = 0;

    while (pos >= 0 && pos < len) {
        uint8_t l = msg[pos];
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len) {
                return -1;
            }
            if (!followed) {
                consumed += 2;
                followed = 1;
            }
            if (++jumps > 16) {
                return -1;
            }
            pos = ((l & 0x3F) << 8) | msg[pos + 1];
            continue;
        }
        if (l == 0) {
            if (!followed) {
                consumed += 1;
            }
            if (out && written < out_cap) {
                out[written] = '\0';
            } else if (out) {
                return -1;
            }
            return consumed;
        }
        if (l > 63 || pos + 1 + l > len) {
            return -1;
        }
        if (!followed) {
            consumed += 1 + l;
        }
        if (out) {
            if (written && written < out_cap) {
                out[written++] = '.';
            }
            if (written + l >= out_cap) {
                return -1;
            }
            for (int i = 0; i < l; i++) {
                out[written++] = (char)msg[pos + 1 + i];
            }
        }
        pos += 1 + l;
    }
    return -1;
}

static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = (char)(cb - 'A' + 'a'); }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return !*a && !*b;
}

int dns_build_query(const char *name, uint16_t id, uint8_t *buf, int cap) {
    if (!name || !buf || cap < 18) {
        return -1;
    }
    buf[0] = (uint8_t)(id >> 8);
    buf[1] = (uint8_t)id;
    buf[2] = 0x01;
    buf[3] = 0x00;
    buf[4] = 0; buf[5] = 1;
    buf[6] = 0; buf[7] = 0;
    buf[8] = 0; buf[9] = 0;
    buf[10] = 0; buf[11] = 0;
    int n = encode_name(name, buf + 12, cap - 12);
    if (n < 0 || 12 + n + 4 > cap) {
        return -1;
    }
    int p = 12 + n;
    buf[p++] = 0; buf[p++] = DNS_TYPE_A;
    buf[p++] = 0; buf[p++] = DNS_CLASS_IN;
    return p;
}

int dns_parse_response(const uint8_t *msg, int len, uint16_t expect_id,
                        const char *expect_name, uint32_t *out) {
    if (!msg || len < 12 || !out) {
        return -4;
    }
    uint16_t id = (uint16_t)((msg[0] << 8) | msg[1]);
    if (id != expect_id) {
        return -4;
    }
    if (!(msg[2] & 0x80)) {
        return -4;
    }
    int rcode = msg[3] & 0x0F;
    if (rcode != 0) {
        return -3;
    }
    int qdcount = (msg[4] << 8) | msg[5];
    int ancount = (msg[6] << 8) | msg[7];
    if (qdcount != 1 || ancount < 1) {
        return -4;
    }

    int pos = 12;
    char qname[DNS_MAX_NAME + 1];
    int n = decode_name(msg, len, pos, qname, sizeof(qname));
    if (n < 0) {
        return -4;
    }
    pos += n;
    if (pos + 4 > len) {
        return -4;
    }
    pos += 4;
    if (expect_name && !name_eq(qname, expect_name)) {
        return -4;
    }

    char want[DNS_MAX_NAME + 1];
    strncpy(want, qname, sizeof(want) - 1);
    want[sizeof(want) - 1] = '\0';
    int cnames = 0;

    for (int i = 0; i < ancount; i++) {
        char rname[DNS_MAX_NAME + 1];
        int rn = decode_name(msg, len, pos, rname, sizeof(rname));
        if (rn < 0) {
            return -4;
        }
        pos += rn;
        if (pos + 10 > len) {
            return -4;
        }
        int rtype = (msg[pos] << 8) | msg[pos + 1];
        int rclass = (msg[pos + 2] << 8) | msg[pos + 3];
        int rdlen = (msg[pos + 8] << 8) | msg[pos + 9];
        pos += 10;
        if (pos + rdlen > len) {
            return -4;
        }
        if (rclass == DNS_CLASS_IN && rtype == DNS_TYPE_A && rdlen == 4 &&
            name_eq(rname, want)) {
            *out = ((uint32_t)msg[pos] << 24) | ((uint32_t)msg[pos + 1] << 16) |
                   ((uint32_t)msg[pos + 2] << 8) | (uint32_t)msg[pos + 3];
            return 0;
        }
        if (rclass == DNS_CLASS_IN && rtype == DNS_TYPE_CNAME && name_eq(rname, want)) {
            if (++cnames > DNS_MAX_CNAME) {
                return -4;
            }
            if (decode_name(msg, len, pos, want, sizeof(want)) < 0) {
                return -4;
            }
        }
        pos += rdlen;
    }
    return -4;
}

#define DNS_CACHE_ENTRIES 8

static struct {
    char name[DNS_MAX_NAME + 1];
    uint32_t ip;
    long expires_ms;
    int used;
} cache[DNS_CACHE_ENTRIES];

static int cache_lookup(const char *name, uint32_t *out) {
    long now = sys_uptime_ms();
    for (int i = 0; i < DNS_CACHE_ENTRIES; i++) {
        if (cache[i].used && now < cache[i].expires_ms && name_eq(cache[i].name, name)) {
            *out = cache[i].ip;
            return 1;
        }
    }
    return 0;
}

static void cache_store(const char *name, uint32_t ip, long ttl_ms) {
    long now = sys_uptime_ms();
    int slot = -1;
    for (int i = 0; i < DNS_CACHE_ENTRIES; i++) {
        if (!cache[i].used || now >= cache[i].expires_ms) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    strncpy(cache[slot].name, name, sizeof(cache[slot].name) - 1);
    cache[slot].name[sizeof(cache[slot].name) - 1] = '\0';
    cache[slot].ip = ip;
    cache[slot].expires_ms = now + ttl_ms;
    cache[slot].used = 1;
}

static int parse_dotted_quad(const char *p, const char *end, uint32_t *out) {
    uint32_t ip = 0;
    int octets = 0;
    while (octets < 4) {
        if (p >= end || *p < '0' || *p > '9') {
            return 0;
        }
        uint32_t v = 0;
        int digits = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p - '0');
            p++;
            if (++digits > 3 || v > 255) {
                return 0;
            }
        }
        ip = (ip << 8) | v;
        octets++;
        if (octets < 4) {
            if (p >= end || *p != '.') {
                return 0;
            }
            p++;
        }
    }
    if (p != end) {
        return 0;
    }
    *out = ip;
    return 1;
}

int dns_parse_resolv_conf(const char *text, int len, uint32_t *out, int max) {
    if (!text || !out || max <= 0) {
        return 0;
    }
    int n = 0;
    int i = 0;
    while (i < len && n < max) {
        int start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        int end = i;
        if (i < len) {
            i++;
        }
        for (int k = start; k < end; k++) {
            if (text[k] == '#' || text[k] == ';') {
                end = k;
                break;
            }
        }
        while (start < end && (text[start] == ' ' || text[start] == '\t' ||
                               text[start] == '\r')) {
            start++;
        }
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                               text[end - 1] == '\r')) {
            end--;
        }
        static const char KEY[] = "nameserver";
        const int KEYLEN = (int)(sizeof(KEY) - 1);
        if (end - start <= KEYLEN) {
            continue;
        }
        int match = 1;
        for (int k = 0; k < KEYLEN; k++) {
            if (text[start + k] != KEY[k]) {
                match = 0;
                break;
            }
        }
        if (!match) {
            continue;
        }
        int v = start + KEYLEN;
        if (text[v] != ' ' && text[v] != '\t') {
            continue;
        }
        while (v < end && (text[v] == ' ' || text[v] == '\t')) {
            v++;
        }
        uint32_t ip = 0;
        if (!parse_dotted_quad(text + v, text + end, &ip) || ip == 0) {
            continue;
        }
        int dup = 0;
        for (int k = 0; k < n; k++) {
            if (out[k] == ip) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            out[n++] = ip;
        }
    }
    return n;
}

int dns_servers(uint32_t *out, int max) {
    if (!out || max <= 0) {
        return 0;
    }
    int n = 0;

    char text[2048];
    long got = sys_readfile(PATH_RESOLV_CONF, text, sizeof(text));
    if (got > (long)sizeof(text)) {
        got = (long)sizeof(text);
    }
    if (got > 0) {
        n = dns_parse_resolv_conf(text, (int)got, out, max);
    }

    os_netconf_t conf;
    if (n < max && sys_netconf(&conf) == 0 && conf.dns != 0) {
        int dup = 0;
        for (int k = 0; k < n; k++) {
            if (out[k] == conf.dns) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            out[n++] = conf.dns;
        }
    }
    return n;
}

void dns_cache_clear(void) {
    for (int i = 0; i < DNS_CACHE_ENTRIES; i++) {
        cache[i].used = 0;
    }
}

int dns_resolve(const char *name, uint32_t *out) {
    if (!name || !out || !name[0]) {
        return -1;
    }
    if (cache_lookup(name, out)) {
        return 0;
    }

    uint32_t servers[DNS_MAX_SERVERS];
    int nservers = dns_servers(servers, DNS_MAX_SERVERS);
    if (nservers == 0) {
        return -1;
    }

    int fd = (int)sys_socket(OS_SOCK_DGRAM);
    if (fd < 0) {
        return -1;
    }
    if (sys_bind(fd, 0) <= 0) {
        sys_close(fd);
        return -1;
    }

    uint16_t id = (uint16_t)(sys_uptime_ms() ^ (sys_getpid() << 8));

    uint8_t query[DNS_MAX_MSG];
    int qlen = dns_build_query(name, id, query, sizeof(query));
    if (qlen < 0) {
        sys_close(fd);
        return -1;
    }

    long deadline = sys_uptime_ms() + DNS_TIMEOUT_MS;
    long next_send = 0;
    uint8_t reply[DNS_MAX_MSG];

    while (sys_uptime_ms() < deadline) {
        if (sys_uptime_ms() >= next_send) {
            for (int i = 0; i < nservers; i++) {
                sys_sendto(fd, servers[i], DNS_PORT, query, (uint32_t)qlen);
            }
            next_send = sys_uptime_ms() + 1000;
        }
        if (sys_sockpoll(fd) > 0) {
            os_sockaddr_t from;
            long n = sys_recvfrom(fd, reply, sizeof(reply), &from);
            if (n > 0) {
                int asked = 0;
                for (int i = 0; i < nservers; i++) {
                    if (from.ip == servers[i]) {
                        asked = 1;
                        break;
                    }
                }
                if (asked) {
                    int rc = dns_parse_response(reply, (int)n, id, name, out);
                    if (rc == 0) {
                        cache_store(name, *out, 60000);
                        sys_close(fd);
                        return 0;
                    }
                    if (rc == -3) {
                        sys_close(fd);
                        return -3;
                    }
                }
            }
        }
        sys_yield();
    }
    sys_close(fd);
    return -2;
}
