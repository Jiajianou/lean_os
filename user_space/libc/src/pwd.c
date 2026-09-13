#include <pwd.h>

#include <errno.h>
#include <string.h>

static char name_root[]  = "root";
static char no_password[] = "";
static char gecos[]      = "";
static char home[]       = "/";
static char shell[]      = "/bin/sh";

static struct passwd the_only_user = {
    name_root, no_password, 0, 0, gecos, home, shell,
};

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
        return 0;
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
