#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_FRAMEBUFFER   (1u << 0)

#define CAP_KILL_ANY      (1u << 1)

#define CAP_POWER         (1u << 2)

#define CAP_CLIPBOARD     (1u << 3)

#define CAP_NETWORK       (1u << 4)

#define CAP_AUDIO         (1u << 5)

#define CAP_DISPLAY_MODE  (1u << 6)

#define CAP_PROCESS_LIST  (1u << 7)

#define CAP_FS_WRITE      (1u << 8)

#define CAP_SET_TIME      (1u << 9)

#define CAP_SYSLOG        (1u << 10)

#define CAP_PKG_ADMIN     (1u << 11)

#define CAP_ALL           0xFFFu

#define CAP_PKG_MAX       (CAP_FS_WRITE | CAP_NETWORK | CAP_AUDIO)

#define CAP_PKG_UNLISTED  0u

#define CAP_APP_DEFAULT   (CAP_FS_WRITE)

typedef struct {
    uint32_t bit;
    const char *name;
} cap_name_t;

static const cap_name_t CAP_NAMES[] = {
    {CAP_FRAMEBUFFER,  "framebuffer"},
    {CAP_KILL_ANY,     "kill-any"},
    {CAP_POWER,        "power"},
    {CAP_CLIPBOARD,    "clipboard"},
    {CAP_NETWORK,      "network"},
    {CAP_AUDIO,        "audio"},
    {CAP_DISPLAY_MODE, "display-mode"},
    {CAP_PROCESS_LIST, "process-list"},
    {CAP_FS_WRITE,     "fs-write"},
    {CAP_SET_TIME,     "set-time"},
    {CAP_SYSLOG,       "syslog"},
};

#define CAP_NAME_COUNT ((int)(sizeof(CAP_NAMES) / sizeof(CAP_NAMES[0])))

typedef struct {
    const char *name;
    uint32_t caps;
} cap_grant_t;

static const cap_grant_t CAP_GRANTS[] = {
    {"init",         CAP_ALL},
    {"compositor",   CAP_ALL},
    {"shutdown",      CAP_APP_DEFAULT | CAP_POWER},
    {"reboot",        CAP_APP_DEFAULT | CAP_POWER},
    {"task_manager",  CAP_APP_DEFAULT | CAP_PROCESS_LIST | CAP_KILL_ANY},
    {"profile",       CAP_APP_DEFAULT | CAP_PROCESS_LIST},
    {"proftest",      CAP_APP_DEFAULT | CAP_PROCESS_LIST},
    {"settings",      CAP_APP_DEFAULT | CAP_DISPLAY_MODE | CAP_CLIPBOARD},
    {"wifi",          CAP_APP_DEFAULT | CAP_NETWORK},
    /* M207: the taskbar opens the Wi-Fi wizard, and a child holds only what
       its parent holds - so the taskbar holds network, and nothing else
       beyond an ordinary application's. */
    {"desktop_shell", CAP_APP_DEFAULT | CAP_NETWORK},
    {"sh",            CAP_ALL},
    {"gui_terminal",  CAP_ALL},
    {"desktop_icons", CAP_ALL},
    {"nettest",       CAP_APP_DEFAULT | CAP_NETWORK},
    {"tcptest",       CAP_APP_DEFAULT | CAP_NETWORK},
    {"netrecv",       CAP_APP_DEFAULT | CAP_NETWORK},
    {"racetest",      CAP_APP_DEFAULT | CAP_NETWORK},
    {"exhausttest",   CAP_APP_DEFAULT | CAP_NETWORK},
    {"nettime",       CAP_APP_DEFAULT | CAP_NETWORK | CAP_SET_TIME},
    {"nslookup",      CAP_APP_DEFAULT | CAP_NETWORK},
    {"fetch",         CAP_APP_DEFAULT | CAP_NETWORK},
    {"httpd",         CAP_APP_DEFAULT | CAP_NETWORK},
    {"ssl_client2",   CAP_APP_DEFAULT | CAP_NETWORK},
    {"ssl_server2",   CAP_APP_DEFAULT | CAP_NETWORK},
    {"httpsget",      CAP_APP_DEFAULT | CAP_NETWORK},
    {"netsurf",       CAP_APP_DEFAULT | CAP_NETWORK},
    {"chromiumnet",   CAP_APP_DEFAULT | CAP_NETWORK},
    {"chromiumcontent", CAP_APP_DEFAULT | CAP_NETWORK},
    {"chromiumshell", CAP_APP_DEFAULT | CAP_NETWORK},
    {"chrome",        CAP_APP_DEFAULT | CAP_NETWORK},
    {"browser",       CAP_APP_DEFAULT | CAP_NETWORK},
    {"audiograb",     CAP_APP_DEFAULT | CAP_AUDIO},
    {"text_editor",   CAP_APP_DEFAULT | CAP_CLIPBOARD},
    {"badptr",        CAP_ALL},
    {"wm_crash",      CAP_APP_DEFAULT | CAP_PROCESS_LIST | CAP_KILL_ANY},
    {"console",       CAP_APP_DEFAULT | CAP_SYSLOG},
    {"captest",       CAP_APP_DEFAULT},
    {"os",            CAP_APP_DEFAULT | CAP_PKG_ADMIN},
    {"pkgtest",       CAP_APP_DEFAULT | CAP_PKG_ADMIN},
};

#define CAP_GRANT_COUNT ((int)(sizeof(CAP_GRANTS) / sizeof(CAP_GRANTS[0])))

static inline uint32_t caps_for_program(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    for (int i = 0; i < CAP_GRANT_COUNT; i++) {
        const char *a = CAP_GRANTS[i].name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            return CAP_GRANTS[i].caps;
        }
    }
    return CAP_APP_DEFAULT;
}

#define PKG_ROOT     "/pkg"
#define PKG_ROOT_LENGTH 4

static inline int path_is_under_pkg(const char *path) {
    if (!path || path[0] != '/') {
        return 0;
    }
    for (int i = 0; i < PKG_ROOT_LENGTH; i++) {
        if (path[i] != PKG_ROOT[i]) {
            return 0;
        }
    }
    return path[PKG_ROOT_LENGTH] == '\0' || path[PKG_ROOT_LENGTH] == '/';
}

#ifdef __cplusplus
}
#endif
