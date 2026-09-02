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

#include <stddef.h>
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

/* M89: set the supplementary list from a user's group memberships.
 * Succeeds for `root`/gid 0, which is already the process's list, and
 * fails for anything else - the same shape setuid takes and for the same
 * reason. */
int initgroups(const char *user, gid_t group);
int setgroups(size_t size, const gid_t *list);

/* M89: the reentrant forms, which toybox calls rather than the plain
 * ones because it caches the results in its own buffer. `buf`/`buflen`
 * hold the strings; `*result` is `out` on success and NULL for "no such
 * group", which is the one part of this interface everybody gets wrong -
 * a NULL result with a 0 return is not an error. */
int getgrgid_r(gid_t gid, struct group *out, char *buf, size_t buflen,
               struct group **result);
int getgrnam_r(const char *name, struct group *out, char *buf, size_t buflen,
               struct group **result);
