/* user_space/libc/include/grp.h - M89
 *
 * The group database, which on this machine is <pwd.h>'s argument
 * repeated: there is one principal, it is in one group, and that group
 * is called `root` with gid 0.
 *
 * Every other gid is "no such group", which is a fact rather than a
 * stub - see <pwd.h> for why a machine with one principal reporting one
 * principal is not the fiction M65 refused.
 */
#pragma once

#include <sys/types.h>

struct group {
    char  *gr_name;
    char  *gr_passwd;
    gid_t  gr_gid;
    char **gr_mem;  /* NULL-terminated; `root` is the only member */
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
void          setgrent(void);
struct group *getgrent(void);
void          endgrent(void);

/* The supplementary-group list, which has one entry here for the same
 * reason. A caller passing a smaller `count` gets -1 with the required
 * size in *count, which is the contract every implementation uses. */
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *count);
int getgroups(int size, gid_t *list);
