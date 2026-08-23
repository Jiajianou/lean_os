#include "net.h"

#include "arp.h"
#include "drivers/klog.h"
#include "drivers/rtl8139.h"

int net_init(void) {
    if (!rtl8139_init()) {
        return 0;
    }
    arp_init();
    klog_puts("[net] local IP 0x");
    klog_put_hex32(NET_LOCAL_IP);
    klog_puts(", gateway 0x");
    klog_put_hex32(NET_GATEWAY_IP);
    klog_putc('\n');
    return 1;
}

const uint8_t *net_local_mac(void) {
    return rtl8139_mac();
}
