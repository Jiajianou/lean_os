#include <stdio.h>

#include "os_net.h"
#include "syscall_wrappers.h"

int main(void) {
    os_netconf_t conf;
    if (sys_netconf(&conf) != 0) {
        printf("netconf: no network interface on this machine\n");
        return 1;
    }

    char buf[16];
    printf("address  %s\n", os_ip_to_string(conf.ip, buf));
    printf("netmask  %s\n", os_ip_to_string(conf.mask, buf));
    printf("gateway  %s\n", os_ip_to_string(conf.gateway, buf));
    printf("dns      %s\n", os_ip_to_string(conf.dns, buf));
    printf("source   %s\n", conf.leased ? "DHCP lease" : "fallback - nothing answered");
    return 0;
}
