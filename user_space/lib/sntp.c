#include "sntp.h"

#include "syscall_wrappers.h"

#define NTP_PORT 123
#define NTP_PACKET_LEN 48

/* Seconds between 1900-01-01 (the NTP epoch) and 1970-01-01 (the Unix
 * one). Seventy years, seventeen of them leap. */
#define NTP_TO_UNIX 2208988800u

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

sntp_result_t sntp_query(uint32_t server_ip, uint32_t timeout_ms, uint32_t *unix_seconds) {
    int fd = (int)sys_socket(OS_SOCK_DGRAM);
    if (fd < 0) {
        return SNTP_NO_SOCKET;
    }
    /* An ephemeral source port, which is what sys_bind(fd, 0) is for -
     * a client that picked 123 for itself would collide with the next
     * one on the same machine and would look, to a server, like another
     * server. */
    if (sys_bind(fd, 0) < 0) {
        sys_close(fd);
        return SNTP_NO_SOCKET;
    }

    uint8_t packet[NTP_PACKET_LEN];
    for (int i = 0; i < NTP_PACKET_LEN; i++) {
        packet[i] = 0;
    }
    /* LI 0 (no warning), VN 3, Mode 3 (client). Version 3 rather than 4
     * because every server answers a v3 request and some very old ones
     * do not answer a v4 one; nothing here uses a v4-only field. */
    packet[0] = 0x1B;

    if (sys_sendto(fd, server_ip, NTP_PORT, packet, NTP_PACKET_LEN) < 0) {
        sys_close(fd);
        return SNTP_SEND_FAILED;
    }

    /* sys_yield against a real deadline rather than a fixed number of
     * spins: a spin count is a duration that changes with the machine,
     * and "wait one second for a time server" is the one thing here that
     * genuinely means a second. */
    sntp_result_t result = SNTP_TIMED_OUT;
    long deadline = sys_uptime_ms() + (long)timeout_ms;
    while (sys_uptime_ms() < deadline) {
        if (sys_sockpoll(fd) > 0) {
            uint8_t reply[NTP_PACKET_LEN];
            os_sockaddr_t from;
            long n = sys_recvfrom(fd, reply, sizeof(reply), &from);
            if (n < NTP_PACKET_LEN || from.ip != server_ip) {
                result = SNTP_BAD_REPLY;
                break;
            }
            /* Mode 4 is "server"; anything else answering on this port
             * is not replying to us. And LI 3 means the server itself is
             * unsynchronised, which is a reply worth refusing rather
             * than a time worth believing. */
            uint8_t li = (uint8_t)(reply[0] >> 6);
            uint8_t mode = (uint8_t)(reply[0] & 0x07);
            uint32_t transmit = read_be32(reply + 40);
            if (mode != 4 || li == 3 || transmit < NTP_TO_UNIX) {
                result = SNTP_BAD_REPLY;
                break;
            }
            *unix_seconds = transmit - NTP_TO_UNIX;
            result = SNTP_OK;
            break;
        }
        sys_yield();
    }

    sys_close(fd);
    return result;
}

const char *sntp_strerror(sntp_result_t r) {
    switch (r) {
    case SNTP_OK:           return "ok";
    case SNTP_NO_SOCKET:    return "could not open a socket";
    case SNTP_SEND_FAILED:  return "the server is unreachable";
    case SNTP_TIMED_OUT:    return "no reply";
    case SNTP_BAD_REPLY:    return "the reply was not a usable time";
    }
    return "unknown";
}
