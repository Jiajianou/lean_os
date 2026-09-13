#include "dhcp.h"

#include "drivers/pit.h"
#include "ethernet.h"
#include "lib/libk.h"
#include "net.h"
#include "socket.h"
#include "udp.h"
#include "wire.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define BOOTREQUEST 1
#define BOOTREPLY   2

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define OPT_PAD            0
#define OPT_SUBNET_MASK    1
#define OPT_ROUTER         3
#define OPT_DNS            6
#define OPT_REQUESTED_IP  50
#define OPT_MESSAGE_TYPE  53
#define OPT_SERVER_ID     54
#define OPT_PARAM_LIST    55
#define OPT_END          255

#define BOOTP_FIXED_LEN 236
#define DHCP_MIN_LEN    (BOOTP_FIXED_LEN + 4)

static const uint8_t *find_option(const uint8_t *msg, uint16_t len, uint8_t code, uint8_t *len_out) {
    uint16_t i = DHCP_MIN_LEN;
    while (i < len) {
        uint8_t opt = msg[i];
        if (opt == OPT_END) {
            return (const uint8_t *)0;
        }
        if (opt == OPT_PAD) {
            i++;
            continue;
        }
        if ((uint16_t)(i + 2) > len) {
            return (const uint8_t *)0;
        }
        uint8_t opt_len = msg[i + 1];
        if ((uint32_t)i + 2 + opt_len > len) {
            return (const uint8_t *)0;
        }
        if (opt == code) {
            *len_out = opt_len;
            return msg + i + 2;
        }
        i = (uint16_t)(i + 2 + opt_len);
    }
    return (const uint8_t *)0;
}

static volatile int have_reply;
static volatile uint16_t reply_len;
#define DHCP_MAX_MESSAGE 576
static uint8_t reply[DHCP_MAX_MESSAGE];
static uint32_t our_xid;

static void dhcp_receive(uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint16_t len) {
    (void)src_ip;
    (void)src_port;
    if (have_reply || len < DHCP_MIN_LEN || len > sizeof(reply)) {
        return;
    }
    if (data[0] != BOOTREPLY || net_read_be32(data + 4) != our_xid) {
        return;
    }
    k_memcpy(reply, data, len);
    reply_len = len;
    have_reply = 1;
}

static uint16_t build_message(uint8_t *msg, uint8_t type, uint32_t requested, uint32_t server) {
    k_memset(msg, 0, DHCP_MIN_LEN);
    msg[0] = BOOTREQUEST;
    msg[1] = 1;
    msg[2] = ETH_ADDR_LEN;
    msg[3] = 0;
    net_write_be32(msg + 4, our_xid);
    msg[10] = 0x80;
    k_memcpy(msg + 28, net_local_mac(), ETH_ADDR_LEN);
    net_write_be32(msg + BOOTP_FIXED_LEN, 0x63825363u);

    uint16_t i = DHCP_MIN_LEN;
    msg[i++] = OPT_MESSAGE_TYPE;
    msg[i++] = 1;
    msg[i++] = type;

    if (requested) {
        msg[i++] = OPT_REQUESTED_IP;
        msg[i++] = 4;
        net_write_be32(msg + i, requested);
        i = (uint16_t)(i + 4);
    }
    if (server) {
        msg[i++] = OPT_SERVER_ID;
        msg[i++] = 4;
        net_write_be32(msg + i, server);
        i = (uint16_t)(i + 4);
    }

    msg[i++] = OPT_PARAM_LIST;
    msg[i++] = 3;
    msg[i++] = OPT_SUBNET_MASK;
    msg[i++] = OPT_ROUTER;
    msg[i++] = OPT_DNS;

    msg[i++] = OPT_END;
    return i;
}

static int exchange(const uint8_t *msg, uint16_t len, uint8_t want, uint32_t ms) {
    have_reply = 0;
    if (udp_send_from(0, NET_BROADCAST_IP, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, msg, len) < 0) {
        return 0;
    }
    uint64_t deadline = pit_get_ticks() + (uint64_t)ms * PIT_HZ / 1000;
    while (pit_get_ticks() < deadline) {
        if (have_reply) {
            uint8_t opt_len = 0;
            const uint8_t *type = find_option(reply, reply_len, OPT_MESSAGE_TYPE, &opt_len);
            if (type && opt_len == 1 && type[0] == want) {
                return 1;
            }
            have_reply = 0;
        }
        __asm__ volatile("hlt");
    }
    return 0;
}

static uint32_t option_ip(uint8_t code) {
    uint8_t len = 0;
    const uint8_t *v = find_option(reply, reply_len, code, &len);
    return (v && len >= 4) ? net_read_be32(v) : 0;
}

int dhcp_configure(void) {
    const uint8_t *mac = net_local_mac();
    our_xid = net_read_be32(mac + 2) ^ (uint32_t)pit_get_ticks() ^ 0x1EA5F00Du;

    socket_set_raw_handler(DHCP_CLIENT_PORT, dhcp_receive);

    static uint8_t msg[DHCP_MIN_LEN + 32];
    int ok = 0;

    uint16_t len = build_message(msg, DHCP_DISCOVER, 0, 0);
    if (exchange(msg, len, DHCP_OFFER, 2000)) {
        uint32_t offered = net_read_be32(reply + 16);
        uint32_t server = option_ip(OPT_SERVER_ID);
        if (offered) {
            len = build_message(msg, DHCP_REQUEST, offered, server);
            if (exchange(msg, len, DHCP_ACK, 2000)) {
                uint32_t acked = net_read_be32(reply + 16);
                net_set_config(acked ? acked : offered,
                               option_ip(OPT_SUBNET_MASK),
                               option_ip(OPT_ROUTER),
                               option_ip(OPT_DNS));
                ok = 1;
            }
        }
    }

    socket_set_raw_handler(DHCP_CLIENT_PORT, (socket_raw_handler_t)0);
    return ok;
}
