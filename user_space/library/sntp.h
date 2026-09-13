#pragma once

#include <stdint.h>

typedef enum {
    SNTP_OK = 0,
    SNTP_NO_SOCKET,
    SNTP_SEND_FAILED,
    SNTP_TIMED_OUT,
    SNTP_BAD_REPLY,
} sntp_result_t;

sntp_result_t sntp_query(uint32_t server_ip, uint32_t timeout_ms, uint32_t *unix_seconds);

const char *sntp_strerror(sntp_result_t r);
