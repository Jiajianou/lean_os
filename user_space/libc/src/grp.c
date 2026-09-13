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
        return -1;
    }
    groups[0] = group;
    return 1;
}

static int copy_gr(struct group *out, char *buf, size_t buflen,
                   struct group **result) {
    size_t vec = 2 * sizeof(char *);
    size_t nlen = strlen(name_root) + 1;
    size_t plen = strlen(no_password) + 1;
    if (vec + nlen + plen > buflen) {
        *result = (struct group *)0;
        return ERANGE;
    }
    char **mem = (char **)(void *)buf;
    char *strs = buf + vec;
    memcpy(strs, name_root, nlen);
    memcpy(strs + nlen, no_password, plen);
    mem[0] = strs;
    mem[1] = (char *)0;
    out->gr_name = strs;
    out->gr_passwd = strs + nlen;
    out->gr_gid = 0;
    out->gr_mem = mem;
    *result = out;
    return 0;
}

int getgrgid_r(gid_t gid, struct group *out, char *buf, size_t buflen,
               struct group **result) {
    if (!out || !buf || !result) {
        return EINVAL;
    }
    if (gid != 0) {
        *result = (struct group *)0;
        return 0;
    }
    return copy_gr(out, buf, buflen, result);
}

int getgrnam_r(const char *name, struct group *out, char *buf, size_t buflen,
               struct group **result) {
    if (!out || !buf || !result) {
        return EINVAL;
    }
    if (!name || strcmp(name, name_root) != 0) {
        *result = (struct group *)0;
        return 0;
    }
    return copy_gr(out, buf, buflen, result);
}

int initgroups(const char *user, gid_t group) {
    if (user && strcmp(user, name_root) == 0 && group == 0) {
        return 0;
    }
    errno = EPERM;
    return -1;
}

int setgroups(size_t size, const gid_t *list) {
    if (size == 0 || (size == 1 && list && list[0] == 0)) {
        return 0;
    }
    errno = EPERM;
    return -1;
}
