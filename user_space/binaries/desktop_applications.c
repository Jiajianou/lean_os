#include <stdio.h>
#include <string.h>

#include "application_dispatch.h"
#include "desktop_applications.h"
#include "syscall_wrappers.h"

typedef int (*desktop_application_entry_t)(int selftest);

static const char *const DESKTOP_APPLICATION_NAMES[] = {
    "settings",
    "task_manager",
    "lvgl_demo",
    "wifi",
    "file_manager",
};

static const desktop_application_entry_t DESKTOP_APPLICATION_ENTRIES[] = {
    desktop_application_settings,
    desktop_application_task_manager,
    desktop_application_widgets,
    desktop_application_wireless,
    desktop_application_files,
};

const char *desktop_application_argument;

#define DESKTOP_APPLICATION_COUNT \
    ((int)(sizeof(DESKTOP_APPLICATION_NAMES) / sizeof(DESKTOP_APPLICATION_NAMES[0])))

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
    return DESKTOP_APPLICATION_ENTRIES[index](selftest);
}
