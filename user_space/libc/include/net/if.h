#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ

/* The flag bits getifaddrs(3) reports. Their values are the ones every
   other system uses, because a program that reads them reads them by
   number as often as by name. */
#define IFF_UP          0x0001
#define IFF_BROADCAST   0x0002
#define IFF_DEBUG       0x0004
#define IFF_LOOPBACK    0x0008
#define IFF_POINTOPOINT 0x0010
#define IFF_NOTRAILERS  0x0020
#define IFF_RUNNING     0x0040
#define IFF_NOARP       0x0080
#define IFF_PROMISC     0x0100
#define IFF_ALLMULTI    0x0200
#define IFF_MULTICAST   0x1000

struct if_nameindex {
    unsigned int if_index;
    char        *if_name;
};

unsigned int if_nametoindex(const char *name);
char        *if_indextoname(unsigned int index, char *name);

#ifdef __cplusplus
}
#endif
