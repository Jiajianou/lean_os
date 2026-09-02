/* user_space/libc/include/net/if.h - M89
 *
 * Network interface names.
 *
 * This machine has exactly one interface and it has no name, because
 * nothing here has ever needed to tell two apart - `netconf` reports the
 * address and there is only ever one NIC (kernel/drivers/rtl8139.c). So
 * if_nametoindex answers for index 1 and nothing else, and
 * if_indextoname reports "eth0", which is the name a program will print
 * if it prints anything.
 *
 * IFNAMSIZ is the standard 16 because programs size buffers with it.
 */
#pragma once

#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ

struct if_nameindex {
    unsigned int if_index;
    char        *if_name;
};

unsigned int if_nametoindex(const char *name);
char        *if_indextoname(unsigned int index, char *name);
