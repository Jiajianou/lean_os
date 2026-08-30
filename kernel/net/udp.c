#include "udp.h"

#include "ip.h"
#include "lib/libk.h"
#include "net.h"
#include "socket.h"
#include "wire.h"

/* The one's-complement sum ip.c also computes, but split into
 * accumulate/fold so the pseudo-header and the datagram can go through
 * the same running total without being copied into one buffer first.
 * `sum` carries between calls; fold_checksum finishes it. */
/* RFC 768's pseudo-header: source and destination address, a zero, the
 * protocol number and the UDP length - the fields UDP borrows from IP so
 * a datagram delivered to the wrong host or the wrong protocol fails its
 * checksum instead of being accepted. */
static uint32_t pseudo_header_sum(uint32_t src_ip, uint32_t dst_ip, uint16_t udp_len) {
    uint8_t pseudo[12];
    pseudo[0] = (uint8_t)(src_ip >> 24); pseudo[1] = (uint8_t)(src_ip >> 16);
    pseudo[2] = (uint8_t)(src_ip >> 8);  pseudo[3] = (uint8_t)src_ip;
    pseudo[4] = (uint8_t)(dst_ip >> 24); pseudo[5] = (uint8_t)(dst_ip >> 16);
    pseudo[6] = (uint8_t)(dst_ip >> 8);  pseudo[7] = (uint8_t)dst_ip;
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_UDP;
    net_write_be16(pseudo + 10, udp_len);
    return net_sum16(0, pseudo, sizeof(pseudo));
}

int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
             const uint8_t *payload, uint16_t payload_len) {
    return udp_send_from(net_local_ip(), dst_ip, dst_port, src_port, payload, payload_len);
}

int udp_send_from(uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                  const uint8_t *payload, uint16_t payload_len) {
    if (payload_len > UDP_MAX_PAYLOAD) {
        return -1;
    }

    static uint8_t datagram[UDP_HEADER_LEN + UDP_MAX_PAYLOAD];
    uint16_t udp_len = (uint16_t)(UDP_HEADER_LEN + payload_len);

    net_write_be16(datagram + 0, src_port);
    net_write_be16(datagram + 2, dst_port);
    net_write_be16(datagram + 4, udp_len);
    datagram[6] = 0; /* checksum, filled in below */
    datagram[7] = 0;
    k_memcpy(datagram + UDP_HEADER_LEN, payload, payload_len);

    /* The pseudo-header must be computed over the address that will
     * actually be in the IP header - which for a DHCP DISCOVER is
     * 0.0.0.0, not this interface's guess at its own address. */
    uint32_t sum = pseudo_header_sum(src_ip, dst_ip, udp_len);
    uint16_t csum = net_fold16(net_sum16(sum, datagram, udp_len));
    /* RFC 768: an all-zero checksum means "not computed", so a real
     * result of zero is transmitted as all ones instead. Both are the
     * same number in one's complement; only the wire encoding differs. */
    if (csum == 0) {
        csum = 0xFFFF;
    }
    net_write_be16(datagram + 6, csum);

    return ip_send_from(src_ip, dst_ip, IP_PROTO_UDP, datagram, udp_len);
}

void udp_handle_packet(uint32_t src_ip, uint32_t dst_ip, const uint8_t *payload, uint16_t len) {
    if (len < UDP_HEADER_LEN) {
        return;
    }
    uint16_t udp_len = net_read_be16(payload + 4);
    if (udp_len < UDP_HEADER_LEN || udp_len > len) {
        return;
    }

    uint16_t csum = net_read_be16(payload + 6);
    if (csum != 0) { /* zero means the sender did not compute one */
        uint32_t sum = pseudo_header_sum(src_ip, dst_ip, udp_len);
        if (net_fold16(net_sum16(sum, payload, udp_len)) != 0) {
            return;
        }
    }

    uint16_t src_port = net_read_be16(payload + 0);
    uint16_t dst_port = net_read_be16(payload + 2);
    socket_deliver(dst_port, src_ip, src_port,
                   payload + UDP_HEADER_LEN, (uint16_t)(udp_len - UDP_HEADER_LEN));
}
