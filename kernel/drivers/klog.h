#pragma once

#include <stddef.h>
#include <stdint.h>

#include <stdint.h>

void klog_init(void);
void klog_putc(char c);
void klog_puts(const char *s);

uint64_t klog_begin(void);
void klog_end(uint64_t flags);

void klog_release_console(void);
int klog_console_released(void);

void klog_enter_panic(void);
void klog_put_hex32(uint32_t value);
void klog_put_dec(uint32_t value);
void klog_put_dec_pad(uint32_t value, int width);
void klog_put_hex64(uint64_t value);

typedef enum {
    KLOG_DEBUG = 0,
    KLOG_INFO,
    KLOG_WARN,
    KLOG_ERROR,
} klog_level_t;

void klog_set_level(klog_level_t level);
void klog_log(klog_level_t level, const char *s);
void klog_log_hex32(klog_level_t level, uint32_t value);
void klog_log_hex64(klog_level_t level, uint64_t value);

static inline void klog_debug(const char *s) { klog_log(KLOG_DEBUG, s); }
static inline void klog_info(const char *s) { klog_log(KLOG_INFO, s); }
static inline void klog_warn(const char *s) { klog_log(KLOG_WARN, s); }
static inline void klog_error(const char *s) { klog_log(KLOG_ERROR, s); }

void klog_use_console(void);

size_t klog_read(uint64_t from, char *out, size_t max, uint64_t *next);

uint64_t klog_written_total(void);
