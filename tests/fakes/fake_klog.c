#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAP 65536
static char cap_buf[CAP];
static size_t cap_len;

static void cap_puts(const char *s) {
    size_t n = strlen(s);
    if (cap_len + n >= CAP) {
        n = CAP - 1 - cap_len;
    }
    memcpy(cap_buf + cap_len, s, n);
    cap_len += n;
    cap_buf[cap_len] = '\0';
}

void klog_init(void) { cap_len = 0; cap_buf[0] = '\0'; }
void klog_use_console(void) {}
void klog_putc(char c) { char s[2] = {c, 0}; cap_puts(s); }
void klog_puts(const char *s) { cap_puts(s ? s : "(null)"); }

void klog_put_hex32(uint32_t v) { char b[16]; snprintf(b, sizeof(b), "%08X", v); cap_puts(b); }
void klog_put_hex64(uint64_t v) { char b[24]; snprintf(b, sizeof(b), "%016llX", (unsigned long long)v); cap_puts(b); }
void klog_put_dec(uint32_t v) { char b[16]; snprintf(b, sizeof(b), "%u", v); cap_puts(b); }
void klog_put_dec64(uint64_t v) { char b[24]; snprintf(b, sizeof(b), "%llu", (unsigned long long)v); cap_puts(b); }

void klog_capture_reset(void) { cap_len = 0; cap_buf[0] = '\0'; }
const char *klog_capture(void) { return cap_buf; }
int klog_capture_contains(const char *needle) {
    return needle && strstr(cap_buf, needle) != NULL;
}
