/* user_space/libc/src/grp.c - M89. See <grp.h>, and <pwd.h> for the
 * argument both files rest on: one principal, reported as one. */
#include <grp.h>

#include <errno.h>
#include <string.h>

static char name_root[] = "root";
static char no_password[] = "";
static char *members[] = {name_root, (char *)0};

static struct group the_only_group = {
    name_root, no_password, 0, members,
};

static int enumerated;

struct group *getgrgid(gid_t gid) {
    return gid == 0 ? &the_only_group : (struct group *)0;
}

struct group *getgrnam(const char *name) {
    if (!name || strcmp(name, name_root) != 0) {
        return (struct group *)0;
    }
    return &the_only_group;
}

void setgrent(void) {
    enumerated = 0;
}

struct group *getgrent(void) {
    if (enumerated) {
        return (struct group *)0;
    }
    enumerated = 1;
    return &the_only_group;
}

void endgrent(void) {
    enumerated = 0;
}

int getgroups(int size, gid_t *list) {
    /* One group. A size of 0 is the "how many are there" query, which is
     * the form every caller uses before allocating. */
    if (size == 0) {
        return 1;
    }
    if (!list || size < 1) {
        errno = EINVAL;
        return -1;
    }
    list[0] = 0;
    return 1;
}

int getgrouplist(const char *user, gid_t group, gid_t *groups, int *count) {
    (void)user;
    if (!count) {
        errno = EFAULT;
        return -1;
    }
    int room = *count;
    *count = 1;
    if (room < 1 || !groups) {
        return -1; /* the documented "too small, here is the size you need" */
    }
    groups[0] = group;
    return 1;
}
