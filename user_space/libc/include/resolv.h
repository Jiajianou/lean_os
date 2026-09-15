#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The BIND resolver interface, in its classic shape rather than glibc's.
   Programs that ask a machine "which nameservers am I supposed to use"
   ask here; on this machine the answer is the one M114's resolver already
   gives, which is /etc/resolv.conf plus whatever DHCP handed over. */

#define MAXNS           3
#define MAXDFLSRCH      3
#define MAXDNSRCH       6
#define MAXRESOLVSORT   10

#define RES_TIMEOUT     5
#define RES_MAXNDOTS    15
#define RES_DFLRETRY    2

#define RES_INIT        0x00000001
#define RES_DEBUG       0x00000002
#define RES_USEVC       0x00000008
#define RES_IGNTC       0x00000020
#define RES_RECURSE     0x00000040
#define RES_DEFNAMES    0x00000080
#define RES_STAYOPEN    0x00000100
#define RES_DNSRCH      0x00000200
#define RES_NOALIASES   0x00001000
#define RES_ROTATE      0x00004000
#define RES_NOCHECKNAME 0x00008000
#define RES_USE_EDNS0   0x00100000
#define RES_USE_DNSSEC  0x00200000

#define RES_DEFAULT     (RES_RECURSE | RES_DEFNAMES | RES_DNSRCH)

#define __RES 19991006

struct __res_state {
    int retrans;
    int retry;
    unsigned long options;
    int nscount;
    struct sockaddr_in nsaddr_list[MAXNS];
    unsigned short id;
    char *dnsrch[MAXDNSRCH + 1];
    char defdname[256];
    unsigned long pfcode;
    unsigned int ndots;
    unsigned int nsort;
    /* Where dnsrch points. glibc hides an equivalent buffer behind an
       anonymous union; this one says what it is, because a program that
       copies a res_state around has to be told the strings travel with
       it. */
    char search_storage[MAXDNSRCH][256];
};

typedef struct __res_state *res_state;

extern struct __res_state _res;

int res_init(void);
int res_ninit(res_state statp);
void res_close(void);
void res_nclose(res_state statp);

#ifdef __cplusplus
}
#endif
