#include "http.h"

#include <string.h>

#include "dns.h"
#include "os_network.h"
#include "syscall_wrappers.h"

#define HTTP_CONNECT_MS 5000
#define HTTP_READ_MS    8000
#define HTTP_HEADER_MAX    2048

static int split_url(const char *url, char *host, int host_cap,
                      uint16_t *port, char *path, int path_cap) {
    if (strncmp(url, "https://", 8) == 0) {
        return -1;
    }
    if (strncmp(url, "http://", 7) != 0) {
        return -1;
    }
    const char *p = url + 7;
    int n = 0;
    *port = 80;
    while (*p && *p != '/' && *p != ':' && n < host_cap - 1) {
        host[n++] = *p++;
    }
    host[n] = '\0';
    if (n == 0) {
        return -1;
    }
    if (*p == ':') {
        p++;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p++ - '0');
            if (v > 65535) {
                return -1;
            }
        }
        if (v == 0) {
            return -1;
        }
        *port = (uint16_t)v;
    }
    int m = 0;
    if (*p != '/') {
        path[m++] = '/';
    }
    while (*p && m < path_cap - 1) {
        path[m++] = *p++;
    }
    path[m] = '\0';
    return 0;
}

static int parse_dotted(const char *s, uint32_t *out) {
    uint32_t v = 0;
    int parts = 0;
    while (parts < 4) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        uint32_t oct = 0;
        while (*s >= '0' && *s <= '9') {
            oct = oct * 10 + (uint32_t)(*s++ - '0');
            if (oct > 255) {
                return -1;
            }
        }
        v = (v << 8) | oct;
        parts++;
        if (parts < 4) {
            if (*s != '.') {
                return -1;
            }
            s++;
        }
    }
    if (*s) {
        return -1;
    }
    *out = v;
    return 0;
}

static long read_until_closed(int fd, char *buffer, long cap, long deadline) {
    long got = 0;
    while (sys_uptime_ms() < deadline) {
        long n = sys_receive(fd, buffer + got, (uint32_t)(cap - got));
        if (n < 0) {
            break;
        }
        if (n == 0) {
            sys_yield();
            continue;
        }
        got += n;
        if (got >= cap) {
            break;
        }
    }
    return got;
}

static long dechunk(char *buffer, long length) {
    long in = 0, out = 0;
    while (in < length) {
        long size = 0;
        int digits = 0;
        while (in < length && buffer[in] != '\r' && buffer[in] != '\n') {
            char c = buffer[in++];
            int d;
            if (c >= '0' && c <= '9') { d = c - '0'; }
            else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
            else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
            else if (c == ';') { while (in < length && buffer[in] != '\r' && buffer[in] != '\n') { in++; } break; }
            else { return -1; }
            size = size * 16 + d;
            if (++digits > 8) {
                return -1;
            }
        }
        if (digits == 0) {
            return -1;
        }
        while (in < length && (buffer[in] == '\r' || buffer[in] == '\n')) {
            in++;
        }
        if (size == 0) {
            return out;
        }
        if (in + size > length) {
            return -1;
        }
        for (long i = 0; i < size; i++) {
            buffer[out++] = buffer[in++];
        }
        while (in < length && (buffer[in] == '\r' || buffer[in] == '\n')) {
            in++;
        }
    }
    return out;
}

static int header_has(const char *header, const char *name, const char **val) {
    for (const char *p = header; *p; p++) {
        if (p == header || p[-1] == '\n') {
            const char *a = name;
            const char *b = p;
            while (*a && *b) {
                char ca = *a, callback = *b;
                if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca - 'A' + 'a'); }
                if (callback >= 'A' && callback <= 'Z') { callback = (char)(callback - 'A' + 'a'); }
                if (ca != callback) { break; }
                a++; b++;
            }
            if (!*a) {
                while (*b == ' ' || *b == ':') { b++; }
                *val = b;
                return 1;
            }
        }
    }
    return 0;
}

long http_get(const char *url, char *body, long cap, int *status_out) {
    char current[512];
    strncpy(current, url, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    for (int redirect = 0; redirect <= HTTP_MAX_REDIRECTS; redirect++) {
        char host[256], path[256];
        uint16_t port;
        if (split_url(current, host, sizeof(host), &port, path, sizeof(path)) != 0) {
            return -1;
        }

        uint32_t ip;
        if (parse_dotted(host, &ip) != 0) {
            if (dns_resolve(host, &ip) != 0) {
                return -2;
            }
        }

        int fd = (int)sys_socket(OS_SOCKET_STREAM);
        if (fd < 0) {
            return -3;
        }
        if (sys_connect(fd, ip, port) != 0) {
            sys_close(fd);
            return -3;
        }
        long deadline = sys_uptime_ms() + HTTP_CONNECT_MS;
        while (sys_connstat(fd) == 0 && sys_uptime_ms() < deadline) {
            sys_yield();
        }
        if (sys_connstat(fd) != 1) {
            sys_close(fd);
            return -3;
        }

        char request[768];
        int rn = 0;
        const char *parts[] = {"GET ", path, " HTTP/1.1\r\nHost: ", host,
                                "\r\nConnection: close\r\nUser-Agent: lean_os/1\r\n\r\n"};
        for (int i = 0; i < 5; i++) {
            for (const char *c = parts[i]; *c && rn < (int)sizeof(request) - 1; c++) {
                request[rn++] = *c;
            }
        }
        long sent = 0;
        while (sent < rn) {
            long n = sys_send(fd, request + sent, (uint32_t)(rn - sent));
            if (n < 0) {
                sys_close(fd);
                return -3;
            }
            if (n == 0) {
                sys_yield();
                continue;
            }
            sent += n;
        }

        long got = read_until_closed(fd, body, cap, sys_uptime_ms() + HTTP_READ_MS);
        sys_close(fd);
        if (got <= 0) {
            return -4;
        }

        long header_end = -1;
        for (long i = 0; i + 3 < got; i++) {
            if (body[i] == '\r' && body[i + 1] == '\n' &&
                body[i + 2] == '\r' && body[i + 3] == '\n') {
                header_end = i + 4;
                break;
            }
        }
        if (header_end < 0 || header_end > HTTP_HEADER_MAX) {
            return -4;
        }

        if (strncmp(body, "HTTP/1.", 7) != 0) {
            return -4;
        }
        int status = 0;
        for (long i = 9; i < header_end && body[i] >= '0' && body[i] <= '9'; i++) {
            status = status * 10 + (body[i] - '0');
        }
        if (status_out) {
            *status_out = status;
        }

        char header[HTTP_HEADER_MAX + 1];
        long hlen = header_end < HTTP_HEADER_MAX ? header_end : HTTP_HEADER_MAX;
        memcpy(header, body, (size_t)hlen);
        header[hlen] = '\0';

        if (status >= 300 && status < 400) {
            const char *loc;
            if (!header_has(header, "location", &loc)) {
                return -4;
            }
            int n = 0;
            while (*loc && *loc != '\r' && *loc != '\n' && n < (int)sizeof(current) - 1) {
                current[n++] = *loc++;
            }
            current[n] = '\0';
            continue;
        }

        long blen = got - header_end;
        memmove(body, body + header_end, (size_t)blen);

        const char *te;
        if (header_has(header, "transfer-encoding", &te) && strncmp(te, "chunked", 7) == 0) {
            blen = dechunk(body, blen);
            if (blen < 0) {
                return -4;
            }
        } else {
            const char *cl;
            if (header_has(header, "content-length", &cl)) {
                long want = 0;
                while (*cl >= '0' && *cl <= '9') {
                    want = want * 10 + (*cl++ - '0');
                }
                if (want < blen) {
                    blen = want;
                }
            }
        }
        body[blen < cap ? blen : cap - 1] = '\0';
        return blen;
    }
    return -5;
}
