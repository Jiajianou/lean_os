#include "dhcp.h"

#include "drivers/pit.h"
#include "ethernet.h"
#include "lib/libk.h"
#include "net.h"
#include "socket.h"
#include "udp.h"

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

/* The fixed part of a BOOTP message: op/htype/hlen/hops, xid, secs,
 * flags, four addresses, 16 bytes of chaddr, 64 of sname, 128 of file,
 * then the magic cookie. 240 bytes before the first option. */
#define BOOTP_FIXED_LEN 236
#define DHCP_MIN_LEN    (BOOTP_FIXED_LEN + 4)

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Walks the option block for one code. Returns a pointer to the value
 * and writes its length, or NULL. Bounds-checked at every step: this is
 * parsing bytes off the wire from a machine we have no reason to trust,
 * and an option that claims to be 200 bytes long 8 bytes from the end of
 * the buffer is exactly the shape of a bug worth not having. */
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

/* The reply the RX path parked for us. Written from the NIC IRQ, read
 * from the boot thread; `have_reply` is the flag that orders the two. */
static volatile int have_reply;
static volatile uint16_t reply_len;
/* A full BOOTP message is 236 bytes of fixed fields plus a vendor area
 * that RFC 2131 allows to be 312 bytes, so 548 - and QEMU's SLIRP sends
 * exactly that, padding the options out to the full length. Sizing this
 * buffer at SOCKET_MAX_DATAGRAM (512) is what made the first version of
 * this client silently receive nothing: every reply was one length check
 * too long, and "no DHCP answer" looks identical whether the server
 * never spoke or we threw away what it said. */
#define DHCP_MAX_MESSAGE 576
static uint8_t reply[DHCP_MAX_MESSAGE];
static uint32_t our_xid;

/* Registered with the socket layer for port 68 rather than owning a
 * struct socket: dhcp runs before there is an address, and a socket that
 * survives past the exchange would hold a port nothing needs again. */
static void dhcp_receive(uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint16_t len) {
    (void)src_ip;
    (void)src_port;
    if (have_reply || len < DHCP_MIN_LEN || len > sizeof(reply)) {
        return;
    }
    if (data[0] != BOOTREPLY || read_be32(data + 4) != our_xid) {
        return;
    }
    k_memcpy(reply, data, len);
    reply_len = len;
    have_reply = 1;
}

/* Builds DISCOVER or REQUEST. `requested` and `server` are zero for a
 * DISCOVER and the offer's values for a REQUEST. Returns the length. */
static uint16_t build_message(uint8_t *msg, uint8_t type, uint32_t requested, uint32_t server) {
    k_memset(msg, 0, DHCP_MIN_LEN);
    msg[0] = BOOTREQUEST;
    msg[1] = 1;  /* htype: Ethernet */
    msg[2] = ETH_ADDR_LEN;
    msg[3] = 0;  /* hops */
    write_be32(msg + 4, our_xid);
    /* flags: the broadcast bit. We ask the server to broadcast its reply
     * rather than unicast it to an address we do not have yet and could
     * not ARP for - the alternative is a stack that has to accept a
     * frame for an IP it does not own, which is a much worse thing to
     * teach ip_handle_packet. */
    msg[10] = 0x80;
    k_memcpy(msg + 28, net_local_mac(), ETH_ADDR_LEN);
    write_be32(msg + BOOTP_FIXED_LEN, 0x63825363u); /* the RFC 2131 magic cookie */

    uint16_t i = DHCP_MIN_LEN;
    msg[i++] = OPT_MESSAGE_TYPE;
    msg[i++] = 1;
    msg[i++] = type;

    if (requested) {
        msg[i++] = OPT_REQUESTED_IP;
        msg[i++] = 4;
        write_be32(msg + i, requested);
        i = (uint16_t)(i + 4);
    }
    if (server) {
        msg[i++] = OPT_SERVER_ID;
        msg[i++] = 4;
        write_be32(msg + i, server);
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

/* Sends `msg` and waits up to `ms` for a reply of `want` type. Returns 1
 * with the reply parked in `reply`, or 0. */
static int exchange(const uint8_t *msg, uint16_t len, uint8_t want, uint32_t ms) {
    have_reply = 0;
    /* Source 0.0.0.0, per RFC 2131: a client without a lease has no
     * address, and the fallback constant this kernel would otherwise use
     * is precisely the guess this exchange exists to replace. */
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
            /* Somebody else's message, or a NAK. Keep waiting rather
             * than giving up - the xid check already established it is
             * ours, so a wrong type here is a server declining, and a
             * second server on the segment may still answer. */
            have_reply = 0;
        }
        __asm__ volatile("hlt");
    }
    return 0;
}

static uint32_t option_ip(uint8_t code) {
    uint8_t len = 0;
    const uint8_t *v = find_option(reply, reply_len, code, &len);
    return (v && len >= 4) ? read_be32(v) : 0;
}

int dhcp_configure(void) {
    /* An xid that is not a constant, so two lean_os machines booting on
     * the same segment at the same moment do not answer each other's
     * exchanges. The MAC is the only per-machine value available this
     * early, and the tick count is the only varying one. */
    const uint8_t *mac = net_local_mac();
    our_xid = read_be32(mac + 2) ^ (uint32_t)pit_get_ticks() ^ 0x1EA5F00Du;

    socket_set_raw_handler(DHCP_CLIENT_PORT, dhcp_receive);

    static uint8_t msg[DHCP_MIN_LEN + 32];
    int ok = 0;

    uint16_t len = build_message(msg, DHCP_DISCOVER, 0, 0);
    if (exchange(msg, len, DHCP_OFFER, 2000)) {
        uint32_t offered = read_be32(reply + 16); /* yiaddr */
        uint32_t server = option_ip(OPT_SERVER_ID);
        if (offered) {
            len = build_message(msg, DHCP_REQUEST, offered, server);
            if (exchange(msg, len, DHCP_ACK, 2000)) {
                uint32_t acked = read_be32(reply + 16);
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
