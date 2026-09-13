#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
    char  *pw_name;
    char  *pw_passwd;
    uid_t  pw_uid;
    gid_t  pw_gid;
    char  *pw_gecos;
    char  *pw_dir;
    char  *pw_shell;
};

struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);

void           setpwent(void);
struct passwd *getpwent(void);
void           endpwent(void);

int getpwuid_r(uid_t uid, struct passwd *out, char *buf, size_t buflen,
               struct passwd **result);
int getpwnam_r(const char *name, struct passwd *out, char *buf, size_t buflen,
               struct passwd **result);

#ifdef __cplusplus
}
#endif
