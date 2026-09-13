#include <stdio.h>
#include <string.h>

#include "os_net.h"
#include "os_time.h"
#include "sntp.h"
#include "syscall_wrappers.h"

int main(int argc, char **argv) {
    int set_clock = 0;
    const char *server_arg = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            set_clock = 1;
        } else if (!server_arg) {
            server_arg = argv[i];
        } else {
            printf("usage: nettime [-s] [server-ip]\n");
            return 2;
        }
    }

    uint32_t server;
    if (server_arg) {
        if (!os_ip_from_string(server_arg, &server)) {
            printf("nettime: '%s' is not an IPv4 address\n", server_arg);
            return 2;
        }
    } else {
        os_netconf_t conf;
        if (sys_netconf(&conf) != 0) {
            printf("nettime: no network interface on this machine\n");
            return 1;
        }
        server = conf.gateway;
    }

    char buf[16];
    uint32_t now = 0;
    sntp_result_t r = sntp_query(server, 1000, &now);
    if (r != SNTP_OK) {
        printf("nettime: %s (%s)\n", os_ip_to_string(server, buf), sntp_strerror(r));
        return 1;
    }

    os_datetime_t t;
    os_civil_from_unix(now, &t);
    printf("%s says %04u-%02u-%02u %02u:%02u:%02u UTC\n",
           os_ip_to_string(server, buf),
           t.year, t.month, t.day, t.hour, t.minute, t.second);

    if (set_clock) {
        if (sys_settime(now) != 0) {
            printf("nettime: the system refused that time\n");
            return 1;
        }
        printf("clock set\n");
    }
    return 0;
}
