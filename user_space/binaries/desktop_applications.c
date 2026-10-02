#include <stdio.h>
#include <string.h>

#include "application_dispatch.h"
#include "desktop_applications.h"
#include "syscall_wrappers.h"
#include "window_manager_client.h"

typedef int (*desktop_application_entry_t)(int selftest);

static const char *const DESKTOP_APPLICATION_NAMES[] = {
    "settings",
    "task_manager",
    "lvgl_demo",
    "wifi",
    "file_manager",
    "wallpapertest",
};

static const desktop_application_entry_t DESKTOP_APPLICATION_ENTRIES[] = {
    desktop_application_settings,
    desktop_application_task_manager,
    desktop_application_widgets,
    desktop_application_wireless,
    desktop_application_files,
    desktop_application_wallpaper_selftest,
};

const char *desktop_application_argument;

#define DESKTOP_APPLICATION_COUNT \
    ((int)(sizeof(DESKTOP_APPLICATION_NAMES) / sizeof(DESKTOP_APPLICATION_NAMES[0])))

/* M215. Two Settings windows disagree about the same settings, two Task
   managers about the same tasks, and two Wi-Fi wizards about one radio, so
   opening one of these when it is already open brings that window forward -
   onto its own desktop, out of the taskbar if it was minimised - instead of
   stacking a second. A pane asked for on the way (the Start menu's Display,
   say) is left where Settings looks for it each tick. */
typedef struct {
    const char *name;
    const char *title;
} single_window_t;

static const single_window_t SINGLE_WINDOW[] = {
    {"settings", "Settings"},
    {"task_manager", "Tasks"},
    {"wifi", "Wi-Fi"},
};

static int bring_forward_existing(const char *name) {
    const char *title = 0;
    for (int i = 0; i < (int)(sizeof(SINGLE_WINDOW) / sizeof(SINGLE_WINDOW[0])); i++) {
        if (strcmp(name, SINGLE_WINDOW[i].name) == 0) {
            title = SINGLE_WINDOW[i].title;
        }
    }
    if (!title) {
        return 0;
    }
    static window_manager_query_response_t windows;
    if (window_manager_query_windows(&windows) != 0) {
        return 0;
    }
    for (int i = 0; i < windows.count && i < WINDOW_MANAGER_MAX_ROUTABLE_WINDOWS; i++) {
        if (strcmp(windows.windows[i].title, title) != 0) {
            continue;
        }
        if (desktop_application_argument && desktop_application_argument[0] && strcmp(name, "settings") == 0) {
            FILE *request = fopen(DESKTOP_APPLICATION_SETTINGS_PANE_REQUEST, "w");
            if (request) {
                fputs(desktop_application_argument, request);
                fclose(request);
            }
        }
        window_manager_send_action(windows.windows[i].window_id, WINDOW_MANAGER_ACTION_FOCUS);
        printf("[%s] already open - brought its window forward\n", name);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *invoked = argc > 0 && argv[0] ? argv[0] : "";
    int selftest = 0;
    int index = application_dispatch_index(invoked, DESKTOP_APPLICATION_NAMES,
                                           DESKTOP_APPLICATION_COUNT);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) {
            selftest = 1;
        } else if (index < 0) {
            index = application_dispatch_index(argv[i], DESKTOP_APPLICATION_NAMES,
                                               DESKTOP_APPLICATION_COUNT);
        } else if (!desktop_application_argument) {
            desktop_application_argument = argv[i];
        }
    }

    if (index < 0) {
        printf("desktop_applications: this one binary is every one of these, and the name it\n"
               "is called by chooses which. /bin/settings and /bin/task_manager are symbolic\n"
               "links to it, seeded on first boot.\n");
        for (int i = 0; i < DESKTOP_APPLICATION_COUNT; i++) {
            printf("  %s\n", DESKTOP_APPLICATION_NAMES[i]);
        }
        return 1;
    }
    if (!selftest && bring_forward_existing(DESKTOP_APPLICATION_NAMES[index])) {
        return 0;
    }
    return DESKTOP_APPLICATION_ENTRIES[index](selftest);
}
