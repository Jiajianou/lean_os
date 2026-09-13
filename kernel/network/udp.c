#include "udp.h"

#include "ip.h"
#include "library/kernel_library.h"
#include "network.h"
#include "socket.h"
#include "wire.h"

static uint32_t pseudo_header_sum(uint32_t source_ip, uint32_t destination_ip, uint16_t udp_length) {
    uint8_t pseudo[12];
    pseudo[0] = (uint8_t)(source_ip >> 24); pseudo[1] = (uint8_t)(source_ip >> 16);
    pseudo[2] = (uint8_t)(source_ip >> 8);  pseudo[3] = (uint8_t)source_ip;
    pseudo[4] = (uint8_t)(destination_ip >> 24); pseudo[5] = (uint8_t)(destination_ip >> 16);
    pseudo[6] = (uint8_t)(destination_ip >> 8);  pseudo[7] = (uint8_t)destination_ip;
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_UDP;
    net_write_be16(pseudo + 10, udp_length);
    return net_sum16(0, pseudo, sizeof(pseudo));
}

int udp_send(uint32_t destination_ip, uint16_t destination_port, uint16_t source_port,
             const uint8_t *payload, uint16_t payload_length) {
    return udp_send_from(net_local_ip(), destination_ip, destination_port, source_port, payload, payload_length);
}

int udp_send_from(uint32_t source_ip, uint32_t destination_ip, uint16_t destination_port, uint16_t source_port,
                  const uint8_t *payload, uint16_t payload_length) {
    if (payload_length > UDP_MAX_PAYLOAD) {
        return -1;
    }

    static uint8_t datagram[UDP_HEADER_LENGTH + UDP_MAX_PAYLOAD];
    uint16_t udp_length = (uint16_t)(UDP_HEADER_LENGTH + payload_length);

    net_write_be16(datagram + 0, source_port);
    net_write_be16(datagram + 2, destination_port);
    net_write_be16(datagram + 4, udp_length);
    datagram[6] = 0;
    datagram[7] = 0;
    k_memcpy(datagram + UDP_HEADER_LENGTH, payload, payload_length);

    uint32_t sum = pseudo_header_sum(source_ip, destination_ip, udp_length);
    uint16_t csum = net_fold16(net_sum16(sum, datagram, udp_length));
    if (csum == 0) {
        csum = 0xFFFF;
    }
    net_write_be16(datagram + 6, csum);

    return ip_send_from(source_ip, destination_ip, IP_PROTO_UDP, datagram, udp_length);
}

void udp_handle_packet(uint32_t source_ip, uint32_t destination_ip, const uint8_t *payload, uint16_t length) {
    if (length < UDP_HEADER_LENGTH) {
        return;
    }
    uint16_t udp_length = net_read_be16(payload + 4);
    if (udp_length < UDP_HEADER_LENGTH || udp_length > length) {
        return;
    }

    uint16_t csum = net_read_be16(payload + 6);
    if (csum != 0) {
        uint32_t sum = pseudo_header_sum(source_ip, destination_ip, udp_length);
        if (net_fold16(net_sum16(sum, payload, udp_length)) != 0) {
            return;
        }
    }

    uint16_t source_port = net_read_be16(payload + 0);
    uint16_t destination_port = net_read_be16(payload + 2);
    socket_deliver(destination_port, source_ip, source_port,
                   payload + UDP_HEADER_LENGTH, (uint16_t)(udp_length - UDP_HEADER_LENGTH));
}
