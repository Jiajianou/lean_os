/* user_space/libc/src/pwd.c - M88. See <pwd.h> for why a machine with
 * one principal has a password database with one row in it, and why that
 * is not the thing M65 refused to write. */
#include <pwd.h>

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
