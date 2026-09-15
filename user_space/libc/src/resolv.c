#include <resolv.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "dns.h"

struct __res_state _res;

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* /etc/resolv.conf's search list and ndots. The nameservers do not come
   from here: dns_servers() reads this file AND asks the kernel what DHCP
   handed over, and a resolver that knows only one of those is the failure
   M114 was about. */
static void read_search_list(res_state statp) {
    int fd = open("/etc/resolv.conf", O_RDONLY);
    if (fd < 0) {
        return;
    }
    char text[2048];
    ssize_t got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got <= 0) {
        return;
    }
    text[got] = '\0';

    int searches = 0;
    char *line = text;
    while (*line) {
        char *end = strchr(line, '\n');
        char *next = end ? end + 1 : line + strlen(line);
        if (end) {
            *end = '\0';
        }
        char *hash = strpbrk(line, "#;");
        if (hash) {
            *hash = '\0';
        }
        while (is_space(*line)) {
            line++;
        }
        if (strncmp(line, "search", 6) == 0 && is_space(line[6])) {
            searches = 0;
            char *p = line + 6;
            while (*p && searches < MAXDNSRCH) {
                while (is_space(*p)) {
                    p++;
                }
                if (!*p) {
                    break;
                }
                char *word = p;
                while (*p && !is_space(*p)) {
                    p++;
                }
                size_t length = (size_t)(p - word);
                if (length >= sizeof(statp->search_storage[0])) {
                    length = sizeof(statp->search_storage[0]) - 1;
                }
                memcpy(statp->search_storage[searches], word, length);
                statp->search_storage[searches][length] = '\0';
                statp->dnsrch[searches] = statp->search_storage[searches];
                searches++;
            }
        } else if (strncmp(line, "domain", 6) == 0 && is_space(line[6])) {
            char *p = line + 6;
            while (is_space(*p)) {
                p++;
            }
            char *word = p;
            while (*p && !is_space(*p)) {
                p++;
            }
            size_t length = (size_t)(p - word);
            if (length >= sizeof(statp->defdname)) {
                length = sizeof(statp->defdname) - 1;
            }
            memcpy(statp->defdname, word, length);
            statp->defdname[length] = '\0';
            if (searches == 0 && length > 0) {
                memcpy(statp->search_storage[0], word, length);
                statp->search_storage[0][length] = '\0';
                statp->dnsrch[0] = statp->search_storage[0];
                searches = 1;
            }
        } else if (strncmp(line, "options", 7) == 0 && is_space(line[7])) {
            char *p = line + 7;
            while (*p) {
                while (is_space(*p)) {
                    p++;
                }
                if (!*p) {
                    break;
                }
                if (strncmp(p, "ndots:", 6) == 0) {
                    unsigned int v = 0;
                    const char *d = p + 6;
                    while (*d >= '0' && *d <= '9') {
                        v = v * 10 + (unsigned int)(*d - '0');
                        d++;
                    }
                    statp->ndots = v > RES_MAXNDOTS ? RES_MAXNDOTS : v;
                } else if (strncmp(p, "rotate", 6) == 0) {
                    statp->options |= RES_ROTATE;
                } else if (strncmp(p, "use-vc", 6) == 0) {
                    statp->options |= RES_USEVC;
                }
                while (*p && !is_space(*p)) {
                    p++;
                }
            }
        }
        line = next;
    }
    statp->dnsrch[searches] = (char *)0;
}

int res_ninit(res_state statp) {
    if (!statp) {
        errno = EINVAL;
        return -1;
    }
    memset(statp, 0, sizeof(*statp));
    statp->retrans = RES_TIMEOUT;
    statp->retry = RES_DFLRETRY;
    statp->options = RES_DEFAULT | RES_INIT;
    statp->ndots = 1;

    uint32_t servers[MAXNS];
    int n = dns_servers(servers, MAXNS);
    for (int i = 0; i < n; i++) {
        statp->nsaddr_list[i].sin_family = AF_INET;
        statp->nsaddr_list[i].sin_port = htons(53);
        statp->nsaddr_list[i].sin_addr.s_addr = htonl(servers[i]);
    }
    statp->nscount = n;

    read_search_list(statp);
    return 0;
}

/* Nothing is held open. res_ninit reads a file and closes it, and the
   sockets a query needs belong to the query - so this is the honest
   no-op rather than a stub that pretends something was released. */
void res_nclose(res_state statp) {
    (void)statp;
}

int res_init(void) {
    return res_ninit(&_res);
}

void res_close(void) {
    res_nclose(&_res);
}
