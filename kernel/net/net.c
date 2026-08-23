#include "net.h"

#include "arp.h"
#include "drivers/klog.h"
#include "drivers/rtl8139.h"

void net_init(void) {
    rtl8139_init();
    arp_init();
    klog_puts("[net] local IP 0x");
    klog_put_hex32(NET_LOCAL_IP);
    klog_puts(", gateway 0x");
    klog_put_hex32(NET_GATEWAY_IP);
    klog_putc('\n');
}

const uint8_t *net_local_mac(void) {
    return rtl8139_mac();
}
