#include "sntp.h"

#include "syscall_wrappers.h"

#define NTP_PORT 123
#define NTP_PACKET_LENGTH 48

#define NTP_TO_UNIX 2208988800u

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

sntp_result_t sntp_query(uint32_t server_ip, uint32_t timeout_ms, uint32_t *unix_seconds) {
    int fd = (int)sys_socket(OS_SOCKET_DGRAM);
    if (fd < 0) {
        return SNTP_NO_SOCKET;
    }
    if (sys_bind(fd, 0) < 0) {
        sys_close(fd);
        return SNTP_NO_SOCKET;
    }

    uint8_t packet[NTP_PACKET_LENGTH];
    for (int i = 0; i < NTP_PACKET_LENGTH; i++) {
        packet[i] = 0;
    }
    packet[0] = 0x1B;

    if (sys_sendto(fd, server_ip, NTP_PORT, packet, NTP_PACKET_LENGTH) < 0) {
        sys_close(fd);
        return SNTP_SEND_FAILED;
    }

    sntp_result_t result = SNTP_TIMED_OUT;
    long deadline = sys_uptime_ms() + (long)timeout_ms;
    while (sys_uptime_ms() < deadline) {
        if (sys_sockpoll(fd) > 0) {
            uint8_t reply[NTP_PACKET_LENGTH];
            os_sockaddr_t from;
            long n = sys_recvfrom(fd, reply, sizeof(reply), &from);
            if (n < NTP_PACKET_LENGTH || from.ip != server_ip) {
                result = SNTP_BAD_REPLY;
                break;
            }
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
