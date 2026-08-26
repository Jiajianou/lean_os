/* user_space/lib/sntp.h
 *
 * M64: SNTP (RFC 4330), the simple half of NTP - one request, one reply,
 * take the server's transmit timestamp. No stratum tracking, no drift
 * discipline, no polling interval: this is a program that asks what time
 * it is, not a daemon that keeps a machine's clock honest, and the
 * difference is about four thousand lines.
 *
 * In user space rather than the kernel, deliberately. A kernel SNTP
 * client would be a kernel that opens sockets and parses replies from
 * the network, which is a much larger trusted surface for something a
 * 60-line program can do through the ordinary syscalls - and the whole
 * point of M64 is that the ordinary syscalls are now enough.
 */
#pragma once

#include <stdint.h>

typedef enum {
    SNTP_OK = 0,
    SNTP_NO_SOCKET,    /* the socket table is full, or there is no NIC */
    SNTP_SEND_FAILED,  /* unreachable - no route, or ARP got no answer */
    SNTP_TIMED_OUT,    /* nothing answered inside the deadline */
    SNTP_BAD_REPLY,    /* something answered, but not with a usable timestamp */
} sntp_result_t;

/* Asks `server` for the time and writes it to *unix_seconds. Blocks up to
 * `timeout_ms` in a poll loop with sys_yield - there is no blocking
 * recvfrom in this OS and SYS_recvfrom's comment says why. */
sntp_result_t sntp_query(uint32_t server_ip, uint32_t timeout_ms, uint32_t *unix_seconds);

/* What went wrong, for a program that has to tell somebody. */
const char *sntp_strerror(sntp_result_t r);
