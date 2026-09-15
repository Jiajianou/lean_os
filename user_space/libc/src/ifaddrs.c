#include <ifaddrs.h>

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include "syscall_wrappers.h"

/* This machine has a loopback and one network interface, and the kernel
   knows the second one's address because DHCP or /etc/settings put it
   there. That is the whole answer, so getifaddrs(3) here is that answer
   in the shape every other system reports it in.

   One allocation holds the list, the names and the addresses, so
   freeifaddrs is one free() - which is what a caller that frees only the
   head of the list requires. */

#define INTERFACE_COUNT 2

typedef struct {
    struct ifaddrs entries[INTERFACE_COUNT];
    char names[INTERFACE_COUNT][IFNAMSIZ];
    struct sockaddr_in addresses[INTERFACE_COUNT];
    struct sockaddr_in masks[INTERFACE_COUNT];
    struct sockaddr_in broadcasts[INTERFACE_COUNT];
} interface_list_t;

static void fill_in(struct sockaddr_in *sa, uint32_t host_order) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(host_order);
}

int getifaddrs(struct ifaddrs **out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    interface_list_t *list = (interface_list_t *)calloc(1, sizeof(*list));
    if (!list) {
        errno = ENOMEM;
        return -1;
    }

    os_netconf_t conf;
    memset(&conf, 0, sizeof(conf));
    int have_interface = (sys_netconf(&conf) == 0 && conf.ip != 0);

    int n = 0;

    strcpy(list->names[n], "lo");
    fill_in(&list->addresses[n], 0x7F000001u);
    fill_in(&list->masks[n], 0xFF000000u);
    fill_in(&list->broadcasts[n], 0x7F000001u);
    list->entries[n].ifa_name = list->names[n];
    list->entries[n].ifa_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
    list->entries[n].ifa_addr = (struct sockaddr *)&list->addresses[n];
    list->entries[n].ifa_netmask = (struct sockaddr *)&list->masks[n];
    list->entries[n].ifa_broadaddr = (struct sockaddr *)&list->broadcasts[n];
    n++;

    if (have_interface) {
        uint32_t mask = conf.mask ? conf.mask : 0xFFFFFF00u;
        strcpy(list->names[n], "eth0");
        fill_in(&list->addresses[n], conf.ip);
        fill_in(&list->masks[n], mask);
        fill_in(&list->broadcasts[n], (conf.ip & mask) | ~mask);
        list->entries[n].ifa_name = list->names[n];
        list->entries[n].ifa_flags =
            IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
        list->entries[n].ifa_addr = (struct sockaddr *)&list->addresses[n];
        list->entries[n].ifa_netmask = (struct sockaddr *)&list->masks[n];
        list->entries[n].ifa_broadaddr =
            (struct sockaddr *)&list->broadcasts[n];
        n++;
    }

    for (int i = 0; i + 1 < n; i++) {
        list->entries[i].ifa_next = &list->entries[i + 1];
    }
    *out = &list->entries[0];
    return 0;
}

void freeifaddrs(struct ifaddrs *list) {
    free(list);
}
