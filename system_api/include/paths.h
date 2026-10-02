#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PATH_BIN  "/bin"
#define PATH_DEFAULT "/bin:/usr/bin"
#define PATH_HOME "/home"
#define PATH_ETC  "/etc"
#define PATH_ICONS "/icons"
#define PATH_TEMPORARY  "/tmp"

#define PATH_BIN_DIRECTORY  "/bin/"
#define PATH_HOME_DIRECTORY "/home/"
#define PATH_ETC_DIRECTORY  "/etc/"
#define PATH_ICONS_DIRECTORY "/icons/"
#define PATH_TEMPORARY_DIRECTORY  "/tmp/"

#define PATH_DEV      "/dev"
#define PATH_DEV_DIRECTORY  "/dev/"
#define PATH_PROCESS     "/proc"
#define PATH_PROCESS_DIRECTORY "/proc/"

#define PATH_SETTINGS PATH_ETC_DIRECTORY "settings.conf"

#define PATH_RESOLV_CONF PATH_ETC_DIRECTORY "resolv.conf"

#define PATH_WALLPAPER_PICTURE PATH_ETC_DIRECTORY "wallpaper.picture"
#define PATH_PICTURES PATH_HOME_DIRECTORY "Pictures"

#define PATH_MAX_LENGTH 4096

static inline int path_join(char *out, const char *directory, const char *name) {
    int n = 0;
    for (const char *s = directory; *s; s++) {
        if (n >= PATH_MAX_LENGTH - 1) {
            return -1;
        }
        out[n++] = *s;
    }
    for (const char *s = name; *s; s++) {
        if (n >= PATH_MAX_LENGTH - 1) {
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
