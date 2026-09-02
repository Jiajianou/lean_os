/* user_space/libc/include/pwd.h - M88
 *
 * The password database, on a machine that has one principal.
 *
 * **This is not the fiction M65 refused.** What M65 declined to write
 * was a permission model that pretended to enforce something. This
 * enforces nothing and claims nothing: there is exactly one principal on
 * this machine, its uid is 0, and it is called `root`. A program that
 * asks who it is running as gets a true answer, and a program that asks
 * about any other uid gets NULL - which is what "no such user" means
 * everywhere and is also, here, a fact.
 *
 * The fields that describe things this machine does not have are the
 * honest values rather than plausible ones: there is no password (`x`
 * would imply a shadow file), the home directory is `/` because that is
 * where a process starts, and the shell is `/bin/sh` because that one is
 * true.
 *
 * `getpwuid` is named in M88's own bullet as one of the identity calls a
 * ported program reaches for. It is here rather than in M89 because a
 * build system calls it to find a home directory long before any of
 * toybox's utilities do.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

struct passwd {
    char  *pw_name;
    char  *pw_passwd;
    uid_t  pw_uid;
    gid_t  pw_gid;
    char  *pw_gecos;
    char  *pw_dir;
    char  *pw_shell;
};

/* Both return a pointer to storage this library owns, which the next
 * call may overwrite - the standard contract, and the reason a caller
 * that wants to keep a field copies it. NULL means no such user, and on
 * this machine every uid but 0 and every name but "root" is no such
 * user. */
struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);

/* The enumeration form, over a database with one row in it. A program
 * that walks every user here sees `root` and then the end, which is the
 * truth rather than a stub that reports nothing at all. */
void           setpwent(void);
struct passwd *getpwent(void);
void           endpwent(void);

/* M89: the reentrant forms - see <grp.h> for the one thing about this
 * interface worth stating, which is that a 0 return with *result NULL
 * means "no such user" and is not a failure. */
int getpwuid_r(uid_t uid, struct passwd *out, char *buf, size_t buflen,
               struct passwd **result);
int getpwnam_r(const char *name, struct passwd *out, char *buf, size_t buflen,
               struct passwd **result);
