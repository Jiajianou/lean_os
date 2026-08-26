/* user_space/bin/netconf.c
 *
 * M64: what this machine's network configuration actually is.
 *
 * Small on purpose, and the first thing worth running after M64: it is
 * the difference between "the kernel logged an address at boot" and "a
 * program can ask", and it is what makes the DHCP client visible to a
 * person rather than only to a self-test. The "(DHCP lease)" versus
 * "(fallback - nothing answered)" line is the whole point: on QEMU the
 * two configurations are identical, so without saying which one this is,
 * a completely broken DHCP client would look exactly like a working one.
 */
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
