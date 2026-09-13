#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PATH_BIN  "/bin"
#define PATH_DEFAULT "/bin:/usr/bin"
#define PATH_HOME "/home"
#define PATH_ETC  "/etc"
#define PATH_ICONS "/icons"
#define PATH_TMP  "/tmp"

#define PATH_BIN_DIR  "/bin/"
#define PATH_HOME_DIR "/home/"
#define PATH_ETC_DIR  "/etc/"
#define PATH_ICONS_DIR "/icons/"
#define PATH_TMP_DIR  "/tmp/"

#define PATH_DEV      "/dev"
#define PATH_DEV_DIR  "/dev/"
#define PATH_PROC     "/proc"
#define PATH_PROC_DIR "/proc/"

#define PATH_SETTINGS PATH_ETC_DIR "settings.conf"

#define PATH_RESOLV_CONF PATH_ETC_DIR "resolv.conf"

#define PATH_MAX_LEN 4096

static inline int path_join(char *out, const char *dir, const char *name) {
    int n = 0;
    for (const char *s = dir; *s; s++) {
        if (n >= PATH_MAX_LEN - 1) {
            return -1;
        }
        out[n++] = *s;
    }
    for (const char *s = name; *s; s++) {
        if (n >= PATH_MAX_LEN - 1) {
            return -1;
        }
        out[n++] = *s;
    }
    out[n] = '\0';
    return 0;
}

#ifdef __cplusplus
}
#endif
