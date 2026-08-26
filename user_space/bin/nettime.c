/* user_space/bin/nettime.c
 *
 * M64: asks a time server what time it is, and can set the clock to the
 * answer - "SNTP as the second way to know the time", which is how the
 * stretch-goal entry put it, next to M59's CMOS RTC as the first.
 *
 *   nettime            - ask the gateway (which, on QEMU, will not answer)
 *   nettime 1.2.3.4    - ask that server
 *   nettime -s 1.2.3.4 - ...and set the clock to what it says
 *
 * The default is the gateway rather than a name like pool.ntp.org
 * because there is no resolver in this OS yet, and rather than a
 * hardcoded public address because a program that silently reaches out
 * to somebody else's server is a thing a person should have to type.
 * On the one network this project boots on, the gateway does not run
 * NTP - so the default run prints a clean "no reply", which is exactly
 * the outcome worth being sure is clean.
 */
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
