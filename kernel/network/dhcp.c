#include "dhcp.h"

#include "drivers/pit.h"
#include "ethernet.h"
#include "scheduler/scheduler.h"
#include "library/kernel_library.h"
#include "network.h"
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
#define OPT_PARAMETER_LIST    55
#define OPT_END          255

#define EXCHANGE_WAIT_MS 2000
#define EXCHANGE_POLL_MS 10

#define BOOTP_FIXED_LENGTH 236
#define DHCP_MIN_LENGTH    (BOOTP_FIXED_LENGTH + 4)

static const uint8_t *find_option(const uint8_t *message, uint16_t length, uint8_t code, uint8_t *length_out) {
    uint16_t i = DHCP_MIN_LENGTH;
    while (i < length) {
        uint8_t opt = message[i];
        if (opt == OPT_END) {
            return (const uint8_t *)0;
        }
        if (opt == OPT_PAD) {
            i++;
            continue;
        }
        if ((uint16_t)(i + 2) > length) {
            return (const uint8_t *)0;
        }
        uint8_t opt_length = message[i + 1];
        if ((uint32_t)i + 2 + opt_length > length) {
            return (const uint8_t *)0;
        }
        if (opt == code) {
            *length_out = opt_length;
            return message + i + 2;
        }
        i = (uint16_t)(i + 2 + opt_length);
    }
    return (const uint8_t *)0;
}

static volatile int have_reply;
static volatile uint16_t reply_length;
#define DHCP_MAX_MESSAGE 576
static uint8_t reply[DHCP_MAX_MESSAGE];
static uint32_t our_xid;

static void dhcp_receive(uint32_t source_ip, uint16_t source_port, const uint8_t *data, uint16_t length) {
    (void)source_ip;
    (void)source_port;
    if (have_reply || length < DHCP_MIN_LENGTH || length > sizeof(reply)) {
        return;
    }
    if (data[0] != BOOTREPLY || net_read_be32(data + 4) != our_xid) {
        return;
    }
    k_memcpy(reply, data, length);
    reply_length = length;
    have_reply = 1;
}

static uint16_t build_message(uint8_t *message, uint8_t type, uint32_t requested, uint32_t server) {
    k_memset(message, 0, DHCP_MIN_LENGTH);
    message[0] = BOOTREQUEST;
    message[1] = 1;
    message[2] = ETH_ADDRESS_LENGTH;
    message[3] = 0;
    net_write_be32(message + 4, our_xid);
    message[10] = 0x80;
    k_memcpy(message + 28, net_local_mac(), ETH_ADDRESS_LENGTH);
    net_write_be32(message + BOOTP_FIXED_LENGTH, 0x63825363u);

    uint16_t i = DHCP_MIN_LENGTH;
    message[i++] = OPT_MESSAGE_TYPE;
    message[i++] = 1;
    message[i++] = type;

    if (requested) {
        message[i++] = OPT_REQUESTED_IP;
        message[i++] = 4;
        net_write_be32(message + i, requested);
        i = (uint16_t)(i + 4);
    }
    if (server) {
        message[i++] = OPT_SERVER_ID;
        message[i++] = 4;
        net_write_be32(message + i, server);
        i = (uint16_t)(i + 4);
    }

    message[i++] = OPT_PARAMETER_LIST;
    message[i++] = 3;
    message[i++] = OPT_SUBNET_MASK;
    message[i++] = OPT_ROUTER;
    message[i++] = OPT_DNS;

    message[i++] = OPT_END;
    return i;
}

static dhcp_abandoned_t abandoned;

static int exchange(const uint8_t *message, uint16_t length, uint8_t want, uint32_t ms) {
    have_reply = 0;
    if (udp_send_from(0, NET_BROADCAST_IP, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, message, length) < 0) {
        return 0;
    }
    uint64_t deadline = pit_get_ticks() + (uint64_t)ms * PIT_HZ / 1000;
    while (pit_get_ticks() < deadline) {
        if (abandoned && abandoned()) {
            return 0;
        }
        if (have_reply) {
            uint8_t opt_length = 0;
            const uint8_t *type = find_option(reply, reply_length, OPT_MESSAGE_TYPE, &opt_length);
            if (type && opt_length == 1 && type[0] == want) {
                return 1;
            }
            have_reply = 0;
        }
        /* A task waits asleep. Halting kept the radio's DHCP task on a
           processor at 100% for every second it waited; only the boot, which
           asks before there are tasks, still has to halt. */
        if (scheduler_current()) {
            scheduler_sleep_ms(EXCHANGE_POLL_MS);
        } else {
            __asm__ volatile("hlt");
        }
    }
    return 0;
}

/* The same question asked again, with the same transaction id, the way RFC
   2131 has a client retransmit: one lost frame should not be the difference
   between an address and none. */
static int exchange_retrying(const uint8_t *message, uint16_t length, uint8_t want, uint32_t attempts) {
    for (uint32_t attempt = 0; attempt < attempts && !(abandoned && abandoned()); attempt++) {
        if (exchange(message, length, want, EXCHANGE_WAIT_MS)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t option_ip(uint8_t code) {
    uint8_t length = 0;
    const uint8_t *v = find_option(reply, reply_length, code, &length);
    return (v && length >= 4) ? net_read_be32(v) : 0;
}

int dhcp_configure(void) {
    return dhcp_configure_retrying(1, (dhcp_abandoned_t)0);
}

int dhcp_configure_retrying(uint32_t attempts, dhcp_abandoned_t give_up) {
    abandoned = give_up;
    const uint8_t *mac = net_local_mac();
    our_xid = net_read_be32(mac + 2) ^ (uint32_t)pit_get_ticks() ^ 0x1EA5F00Du;

    socket_set_raw_handler(DHCP_CLIENT_PORT, dhcp_receive);

    static uint8_t message[DHCP_MIN_LENGTH + 32];
    int ok = 0;

    uint16_t length = build_message(message, DHCP_DISCOVER, 0, 0);
    if (exchange_retrying(message, length, DHCP_OFFER, attempts)) {
        uint32_t offered = net_read_be32(reply + 16);
        uint32_t server = option_ip(OPT_SERVER_ID);
        if (offered) {
            length = build_message(message, DHCP_REQUEST, offered, server);
            if (exchange_retrying(message, length, DHCP_ACK, attempts)) {
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
