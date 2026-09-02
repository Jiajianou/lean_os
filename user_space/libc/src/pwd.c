/* user_space/libc/src/pwd.c - M88. See <pwd.h> for why a machine with
 * one principal has a password database with one row in it, and why that
 * is not the thing M65 refused to write. */
#include <pwd.h>

#include <errno.h>
#include <string.h>

/* Not `const`, because `struct passwd`'s members are `char *` - which is
 * the interface every program was written against, so a library that
 * made them `const char *` would be a library those programs do not
 * compile with. Nothing writes through them. */
static char name_root[]  = "root";
static char no_password[] = ""; /* not "x": that would name a shadow file this machine does not have */
static char gecos[]      = "";
static char home[]       = "/";
static char shell[]      = "/bin/sh";

static struct passwd the_only_user = {
    name_root, no_password, 0, 0, gecos, home, shell,
};

/* Where getpwent() has got to. One row, so this is a flag rather than an
 * index, and it says so. */
static int enumerated;

struct passwd *getpwuid(uid_t uid) {
    return uid == 0 ? &the_only_user : (struct passwd *)0;
}

struct passwd *getpwnam(const char *name) {
    if (!name || strcmp(name, name_root) != 0) {
        return (struct passwd *)0;
    }
    return &the_only_user;
}

void setpwent(void) {
    enumerated = 0;
}

struct passwd *getpwent(void) {
    if (enumerated) {
        return (struct passwd *)0;
    }
    enumerated = 1;
    return &the_only_user;
}

void endpwent(void) {
    enumerated = 0;
}

/* ---- M89: the reentrant forms ---------------------------------------
 *
 * They copy the strings into the caller's buffer rather than returning
 * pointers into this file's storage, which is the whole reason they
 * exist - and toybox calls them for exactly that reason: it caches
 * entries and would otherwise be holding pointers that the next lookup
 * overwrites.
 *
 * The contract that surprises people, so it is written twice (here and
 * in <pwd.h>): "no such user" is a return of 0 with *result set to NULL.
 * A non-zero return is an errno value, not -1, and the only one this can
 * produce is ERANGE for a buffer too small.
 */
static int copy_pw(struct passwd *out, char *buf, size_t buflen,
                   struct passwd **result) {
    static const char *const fields[] = {name_root, no_password, gecos, home,
                                         shell};
    char **dst[] = {&out->pw_name, &out->pw_passwd, &out->pw_gecos,
                    &out->pw_dir, &out->pw_shell};
    size_t used = 0;
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        size_t n = strlen(fields[i]) + 1;
        if (used + n > buflen) {
            *result = (struct passwd *)0;
            return ERANGE;
        }
        memcpy(buf + used, fields[i], n);
        *dst[i] = buf + used;
        used += n;
    }
    out->pw_uid = 0;
    out->pw_gid = 0;
    *result = out;
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd *out, char *buf, size_t buflen,
               struct passwd **result) {
    if (!out || !buf || !result) {
        return EINVAL;
    }
    if (uid != 0) {
        *result = (struct passwd *)0;
        return 0; /* no such user - not an error */
    }
    return copy_pw(out, buf, buflen, result);
}

int getpwnam_r(const char *name, struct passwd *out, char *buf, size_t buflen,
               struct passwd **result) {
    if (!out || !buf || !result) {
        return EINVAL;
    }
    if (!name || strcmp(name, name_root) != 0) {
        *result = (struct passwd *)0;
        return 0;
    }
    return copy_pw(out, buf, buflen, result);
}
