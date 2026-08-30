#include "dns.h"

#include <string.h>

#include "os_net.h"
#include "syscall_wrappers.h"

#define DNS_PORT        53
#define DNS_TIMEOUT_MS  3000
#define DNS_MAX_MSG     512  /* RFC 1035's limit for UDP without EDNS0 */
#define DNS_MAX_CNAME   4    /* how many CNAMEs to follow inside one reply */

#define DNS_TYPE_A      1
#define DNS_TYPE_CNAME  5
#define DNS_CLASS_IN    1

/* ---- writing a name ---------------------------------------------------
 *
 * "www.example.com" becomes 3 w w w 7 e x a m p l e 3 c o m 0. Every
 * label is length-prefixed and the whole thing is terminated by a zero
 * length, which is also why a label may not be longer than 63: the top
 * two bits of that byte are reserved for the compression pointer form.
 */
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

/* ---- reading a name ---------------------------------------------------
 *
 * THE part of this parser that has to be right, and the one every
 * from-scratch resolver gets wrong the first time.
 *
 * A label whose top two bits are set is not a label - it is a POINTER to
 * an offset earlier in the message, which is how DNS avoids repeating
 * "example.com" in every record. Two consequences, both hostile:
 *
 *   - a pointer can point at another pointer, so following them is a
 *     loop, and a malicious or broken server can make that loop infinite
 *     by pointing a name at itself. Bounded by a jump budget rather than
 *     by trusting the sender.
 *   - a pointer can point anywhere in the message, including forwards or
 *     into the middle of a record, so every offset is range-checked
 *     against the message rather than assumed.
 *
 * Returns the number of bytes consumed *at the original position* (which
 * is 2 for a pure pointer, however long the name turns out to be), or -1.
 * `out` may be NULL when the caller only needs to skip a name.
 */
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
                return -1; /* a pointer loop - bounded, not trusted */
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
    /* Case-insensitive: a server is free to echo the question back in a
     * different case, and RFC 1035 says names compare that way. */
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
    buf[2] = 0x01; /* recursion desired - we are a stub resolver, not a server */
    buf[3] = 0x00;
    buf[4] = 0; buf[5] = 1; /* one question */
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
        /* Not our exchange. M64 made the same check in the DHCP client
         * and for the same reason: another program's reply on the same
         * wire must not be able to answer our question. */
        return -4;
    }
    if (!(msg[2] & 0x80)) {
        return -4; /* not a response */
    }
    int rcode = msg[3] & 0x0F;
    if (rcode != 0) {
        return -3; /* NXDOMAIN and friends - a real answer, just not one we wanted */
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
    pos += 4; /* qtype + qclass */
    if (expect_name && !name_eq(qname, expect_name)) {
        return -4; /* answered a different question */
    }

    /* Follow CNAMEs within this reply. `want` is the name currently being
     * chased; an A record only counts if it is for that name, which is
     * what stops an answer section from smuggling in an address for
     * something nobody asked about. */
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
                return -4; /* a CNAME chain long enough to be a loop */
            }
            if (decode_name(msg, len, pos, want, sizeof(want)) < 0) {
                return -4;
            }
        }
        pos += rdlen;
    }
    return -4; /* no answer we asked for */
}

/* ---- the cache --------------------------------------------------------
 *
 * Eight entries, expiring on the TTL the server gave rather than on a
 * number invented here. Small on purpose: a desktop resolves a handful of
 * names, and a cache large enough to need eviction policy is a cache with
 * a policy to get wrong.
 */
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
        slot = 0; /* nothing expired - overwrite the first, no policy to get wrong */
    }
    strncpy(cache[slot].name, name, sizeof(cache[slot].name) - 1);
    cache[slot].name[sizeof(cache[slot].name) - 1] = '\0';
    cache[slot].ip = ip;
    cache[slot].expires_ms = now + ttl_ms;
    cache[slot].used = 1;
}

int dns_resolve(const char *name, uint32_t *out) {
    if (!name || !out || !name[0]) {
        return -1;
    }
    if (cache_lookup(name, out)) {
        return 0;
    }

    os_netconf_t conf;
    if (sys_netconf(&conf) != 0 || conf.dns == 0) {
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

    /* The id ties a reply to *this* exchange. Derived from the clock
     * rather than a counter so two runs of the same program do not open
     * with the same number. */
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
        /* Resent on a one-second cadence: UDP loses datagrams and a stub
         * resolver that asked once would report "timed out" for a single
         * dropped packet. */
        if (sys_uptime_ms() >= next_send) {
            sys_sendto(fd, conf.dns, DNS_PORT, query, (uint32_t)qlen);
            next_send = sys_uptime_ms() + 1000;
        }
        if (sys_sockpoll(fd) > 0) {
            os_sockaddr_t from;
            long n = sys_recvfrom(fd, reply, sizeof(reply), &from);
            if (n > 0) {
                /* Only the server we asked. Anyone on this segment can
                 * send us a datagram; only one of them was asked a
                 * question. */
                if (from.ip == conf.dns) {
                    int rc = dns_parse_response(reply, (int)n, id, name, out);
                    if (rc == 0) {
                        cache_store(name, *out, 60000);
                        sys_close(fd);
                        return 0;
                    }
                    if (rc == -3) {
                        sys_close(fd);
                        return -3; /* a definite "no" - do not keep asking */
                    }
                }
            }
        }
        sys_yield();
    }
    sys_close(fd);
    return -2;
}
