#pragma once

#include <stddef.h>
#include <stdint.h>

#include <stdint.h>

void kernel_log_init(void);
void kernel_log_putc(char c);
void kernel_log_puts(const char *s);

uint64_t kernel_log_begin(void);
void kernel_log_end(uint64_t flags);

void kernel_log_release_console(void);

void kernel_log_enter_panic(void);
void kernel_log_put_hex32(uint32_t value);
void kernel_log_put_dec(uint32_t value);
void kernel_log_put_dec_pad(uint32_t value, int width);
void kernel_log_put_hex64(uint64_t value);

typedef enum {
    KERNEL_LOG_DEBUG = 0,
    KERNEL_LOG_INFO,
    KERNEL_LOG_WARN,
    KERNEL_LOG_ERROR,
} kernel_log_level_t;

void kernel_log_log(kernel_log_level_t level, const char *s);
void kernel_log_log_hex64(kernel_log_level_t level, uint64_t value);

static inline void kernel_log_debug(const char *s) { kernel_log_log(KERNEL_LOG_DEBUG, s); }
static inline void kernel_log_info(const char *s) { kernel_log_log(KERNEL_LOG_INFO, s); }
static inline void kernel_log_warn(const char *s) { kernel_log_log(KERNEL_LOG_WARN, s); }
static inline void kernel_log_error(const char *s) { kernel_log_log(KERNEL_LOG_ERROR, s); }

void kernel_log_use_console(void);

size_t kernel_log_read(uint64_t from, char *out, size_t max, uint64_t *next);

uint64_t kernel_log_written_total(void);
