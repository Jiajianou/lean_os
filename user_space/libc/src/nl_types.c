#include <errno.h>
#include <nl_types.h>

nl_catd catopen(const char *name, int flag) {
    (void)name;
    (void)flag;
    errno = ENOENT;
    return (nl_catd)-1;
}

char *catgets(nl_catd catalogue, int set, int message, const char *fallback) {
    (void)catalogue;
    (void)set;
    (void)message;
    return (char *)fallback;
}

int catclose(nl_catd catalogue) {
    if (catalogue == (nl_catd)-1 || catalogue == (nl_catd)0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}
