#include <mntent.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MNT_LINE 512

static struct mntent shared;
static char shared_buf[MNT_LINE];

FILE *setmntent(const char *path, const char *mode) {
    if (!path || !mode) {
        return 0;
    }
    if (mode[0] != 'r') {
        return 0;
    }
    return fopen(path, "r");
}

int endmntent(FILE *f) {
    if (f) {
        fclose(f);
    }
    return 1;
}

static int split(char *line, char **fields, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p || *p == '\n') {
            break;
        }
        fields[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return n;
}

struct mntent *getmntent_r(FILE *f, struct mntent *out, char *buf, int buflen) {
    if (!f || !out || !buf || buflen <= 0) {
        return 0;
    }
    while (fgets(buf, buflen, f)) {
        char *fields[6];
        if (split(buf, fields, 6) != 6) {
            continue;
        }
        out->mnt_fsname = fields[0];
        out->mnt_dir = fields[1];
        out->mnt_type = fields[2];
        out->mnt_opts = fields[3];
        out->mnt_freq = atoi(fields[4]);
        out->mnt_passno = atoi(fields[5]);
        return out;
    }
    return 0;
}

struct mntent *getmntent(FILE *f) {
    return getmntent_r(f, &shared, shared_buf, (int)sizeof(shared_buf));
}

char *hasmntopt(const struct mntent *me, const char *opt) {
    if (!me || !me->mnt_opts || !opt) {
        return 0;
    }
    size_t n = strlen(opt);
    char *p = me->mnt_opts;
    for (;;) {
        if (strncmp(p, opt, n) == 0 && (p[n] == '\0' || p[n] == ',')) {
            return p;
        }
        char *comma = strchr(p, ',');
        if (!comma) {
            return 0;
        }
        p = comma + 1;
    }
}
