#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

static char *put_decimal(char *out, unsigned value) {
    char digits[4];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) {
        *out++ = digits[--n];
    }
    return out;
}

static char *put_dotted(char *out, const uint8_t *bytes) {
    for (int i = 0; i < 4; i++) {
        if (i) {
            *out++ = '.';
        }
        out = put_decimal(out, bytes[i]);
    }
    return out;
}

static char *put_hex_group(char *out, unsigned value) {
    static const char hex[] = "0123456789abcdef";
    int started = 0;
    for (int shift = 12; shift >= 0; shift -= 4) {
        unsigned nibble = (value >> shift) & 0xF;
        if (nibble || started || shift == 0) {
            *out++ = hex[nibble];
            started = 1;
        }
    }
    return out;
}

static const char *finish(const char *text, size_t length, char *destination, socklen_t size) {
    if ((size_t)size <= length) {
        errno = ENOSPC;
        return (const char *)0;
    }
    memcpy(destination, text, length);
    destination[length] = '\0';
    return destination;
}

static const char *ntop4(const uint8_t *bytes, char *destination, socklen_t size) {
    char text[INET_ADDRSTRLEN];
    char *end = put_dotted(text, bytes);
    return finish(text, (size_t)(end - text), destination, size);
}

static const char *ntop6(const uint8_t *bytes, char *destination, socklen_t size) {
    unsigned words[8];
    for (int i = 0; i < 8; i++) {
        words[i] = ((unsigned)bytes[2 * i] << 8) | bytes[2 * i + 1];
    }
    int best_base = -1;
    int best_length = 0;
    int run_base = -1;
    int run_length = 0;
    for (int i = 0; i < 8; i++) {
        if (words[i] == 0) {
            if (run_base < 0) {
                run_base = i;
                run_length = 1;
            } else {
                run_length++;
            }
            if (run_length > best_length) {
                best_base = run_base;
                best_length = run_length;
            }
        } else {
            run_base = -1;
            run_length = 0;
        }
    }
    if (best_length < 2) {
        best_base = -1;
    }
    char text[INET6_ADDRSTRLEN];
    char *out = text;
    for (int i = 0; i < 8; i++) {
        if (best_base >= 0 && i >= best_base && i < best_base + best_length) {
            if (i == best_base) {
                *out++ = ':';
            }
            continue;
        }
        if (i) {
            *out++ = ':';
        }
        if (i == 6 && best_base == 0 &&
            (best_length == 6 || (best_length == 5 && words[5] == 0xFFFF))) {
            out = put_dotted(out, bytes + 12);
            break;
        }
        out = put_hex_group(out, words[i]);
    }
    if (best_base >= 0 && best_base + best_length == 8) {
        *out++ = ':';
    }
    return finish(text, (size_t)(out - text), destination, size);
}

const char *inet_ntop(int af, const void *source, char *destination, socklen_t size) {
    if (!source || !destination) {
        errno = EINVAL;
        return (const char *)0;
    }
    if (af == AF_INET) {
        return ntop4((const uint8_t *)source, destination, size);
    }
    if (af == AF_INET6) {
        return ntop6((const uint8_t *)source, destination, size);
    }
    errno = EAFNOSUPPORT;
    return (const char *)0;
}

static int pton4(const char *source, uint8_t *out) {
    uint8_t bytes[4];
    int octets = 0;
    int digits = 0;
    unsigned value = 0;
    for (const char *p = source;; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            if (digits && value == 0) {
                return 0;
            }
            value = value * 10 + (unsigned)(c - '0');
            if (value > 255 || ++digits > 3) {
                return 0;
            }
            continue;
        }
        if (!digits || octets == 4) {
            return 0;
        }
        bytes[octets++] = (uint8_t)value;
        value = 0;
        digits = 0;
        if (c == '\0') {
            break;
        }
        if (c != '.' || octets == 4) {
            return 0;
        }
    }
    if (octets != 4) {
        return 0;
    }
    memcpy(out, bytes, 4);
    return 1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int pton6(const char *source, uint8_t *out) {
    uint8_t bytes[16];
    memset(bytes, 0, sizeof(bytes));
    int at = 0;
    int gap = -1;
    const char *p = source;
    if (*p == ':') {
        if (p[1] != ':') {
            return 0;
        }
        p++;
    }
    const char *group_start = p;
    unsigned value = 0;
    int digits = 0;
    for (;; p++) {
        char c = *p;
        int h = hex_value(c);
        if (h >= 0) {
            if (++digits > 4) {
                return 0;
            }
            value = (value << 4) | (unsigned)h;
            continue;
        }
        if (c == ':') {
            group_start = p + 1;
            if (!digits) {
                if (gap >= 0) {
                    return 0;
                }
                gap = at;
                continue;
            }
            if (p[1] == '\0' || at + 2 > 16) {
                return 0;
            }
            bytes[at++] = (uint8_t)(value >> 8);
            bytes[at++] = (uint8_t)value;
            value = 0;
            digits = 0;
            continue;
        }
        if (c == '.' && at + 4 <= 16) {
            if (!pton4(group_start, bytes + at)) {
                return 0;
            }
            at += 4;
            digits = 0;
            break;
        }
        if (c != '\0') {
            return 0;
        }
        break;
    }
    if (digits) {
        if (at + 2 > 16) {
            return 0;
        }
        bytes[at++] = (uint8_t)(value >> 8);
        bytes[at++] = (uint8_t)value;
    }
    if (gap >= 0) {
        if (at == 16) {
            return 0;
        }
        int tail = at - gap;
        memmove(bytes + 16 - tail, bytes + gap, (size_t)tail);
        memset(bytes + gap, 0, (size_t)(16 - tail - gap));
        at = 16;
    }
    if (at != 16) {
        return 0;
    }
    memcpy(out, bytes, 16);
    return 1;
}

int inet_pton(int af, const char *source, void *destination) {
    if (!source || !destination) {
        return 0;
    }
    if (af == AF_INET) {
        return pton4(source, (uint8_t *)destination);
    }
    if (af == AF_INET6) {
        return pton6(source, (uint8_t *)destination);
    }
    errno = EAFNOSUPPORT;
    return -1;
}
