#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct group {
    char  *gr_name;
    char  *gr_passwd;
    gid_t  gr_gid;
    char **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
void          setgrent(void);
struct group *getgrent(void);
void          endgrent(void);

int getgrouplist(const char *user, gid_t group, gid_t *groups, int *count);
int getgroups(int size, gid_t *list);

int initgroups(const char *user, gid_t group);
int setgroups(size_t size, const gid_t *list);

int getgrgid_r(gid_t gid, struct group *out, char *buf, size_t buflen,
               struct group **result);
int getgrnam_r(const char *name, struct group *out, char *buf, size_t buflen,
               struct group **result);

#ifdef __cplusplus
}
#endif
