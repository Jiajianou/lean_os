#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAP 65536
static char cap_buffer[CAP];
static size_t cap_length;

static void cap_puts(const char *s) {
    size_t n = strlen(s);
    if (cap_length + n >= CAP) {
        n = CAP - 1 - cap_length;
    }
    memcpy(cap_buffer + cap_length, s, n);
    cap_length += n;
    cap_buffer[cap_length] = '\0';
}

void kernel_log_init(void) { cap_length = 0; cap_buffer[0] = '\0'; }
void kernel_log_use_console(void) {}
void kernel_log_putc(char c) { char s[2] = {c, 0}; cap_puts(s); }
void kernel_log_puts(const char *s) { cap_puts(s ? s : "(null)"); }

void kernel_log_put_hex32(uint32_t v) { char b[16]; snprintf(b, sizeof(b), "%08X", v); cap_puts(b); }
void kernel_log_put_hex64(uint64_t v) { char b[24]; snprintf(b, sizeof(b), "%016llX", (unsigned long long)v); cap_puts(b); }
void kernel_log_put_dec(uint32_t v) { char b[16]; snprintf(b, sizeof(b), "%u", v); cap_puts(b); }
void kernel_log_put_dec64(uint64_t v) { char b[24]; snprintf(b, sizeof(b), "%llu", (unsigned long long)v); cap_puts(b); }

void kernel_log_capture_reset(void) { cap_length = 0; cap_buffer[0] = '\0'; }
const char *kernel_log_capture(void) { return cap_buffer; }
int kernel_log_capture_contains(const char *needle) {
    return needle && strstr(cap_buffer, needle) != NULL;
}
