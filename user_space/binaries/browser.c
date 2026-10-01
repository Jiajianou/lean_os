#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "paths.h"
#include "syscall_wrappers.h"

/* The desktop's Browser: Chromium's own browser, started the way this
   desktop needs it started.

   A desktop icon spawns a program with one argument, and a browser here needs
   six switches before the page - which ozone platform, that there is no GPU,
   that it is one process, the window's size, where its log goes - and a
   program whose right invocation is a line nobody can type is not installed,
   it is lying around. So /bin/browser is the name the desktop knows, and it
   execs /bin/chromiumshell with the switches spelled out once, here.

   The exec keeps this process's capabilities intersected with the manifest
   entry for the path it becomes, which is M166's rule and is why "browser"
   and "chromiumshell" carry the same grant: CAP_APP_DEFAULT | CAP_NETWORK
   and not CAP_FRAMEBUFFER. Twenty-odd megabytes of somebody else's C++
   running JavaScript from a machine nobody here controls paints its own
   window's shared segment, as NetSurf did, and nothing else.

   --in-process-gpu because this machine has no GPU and the viz compositor
   would otherwise live in a GPU process of its own, drawing into a window
   only the browser process has: the ozone platform looks a widget's shared
   segment up in the process that connected it. The renderer is in a process
   of its own, under M166's capability sandbox - M173 got it there, and what
   was in the way was a utility process dying a second after it started,
   which nothing had been able to see until this launcher gave descriptor 2
   somewhere to go. */

#define BROWSER_PROGRAM PATH_BIN_DIRECTORY "chrome"
#define SHELL_PROGRAM PATH_BIN_DIRECTORY "chromiumshell"
#define BROWSER_HOME_PAGE "file:///usr/share/browser/home.html"
#define BROWSER_PROFILE PATH_HOME_DIRECTORY ".config/chromium"

#define BROWSER_FLAGS_FILE PATH_ETC_DIRECTORY "chromium-flags.conf"
#define BROWSER_MAX_FLAGS 16

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* M204. Chromium keeps one browser per profile with a SingletonLock - a
   symbolic link naming the host and the process id that holds it - and a
   browser that never exited cleanly leaves it behind: the machine switched
   off with the window open, or a crash. Process ids here start from the same
   place every boot, so the id in a stale lock is often a live process on the
   next one, and Chromium then hands the launch to an "existing browser
   session" that does not exist and exits. The Browser icon did nothing and
   neither did the session restore. When no browser process is running the
   lock cannot be anybody's, so it goes - and only then, because a second
   browser on one profile is what the lock is for. */
static int browser_is_running(void) {
    DIR *proc = opendir("/proc");
    if (!proc) {
        return 1;
    }
    int running = 0;
    struct dirent *entry;
    while (!running && (entry = readdir(proc)) != 0) {
        const char *name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') {
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        char argv0[sizeof(BROWSER_PROGRAM) + 1];
        long n = read(fd, argv0, sizeof(argv0));
        close(fd);
        if (n >= (long)sizeof(BROWSER_PROGRAM) &&
            memcmp(argv0, BROWSER_PROGRAM, sizeof(BROWSER_PROGRAM)) == 0) {
            running = 1;
        }
    }
    closedir(proc);
    return running;
}

static void forget_stale_profile_lock(void) {
    static const char *const singletons[] = {
        BROWSER_PROFILE "/SingletonLock",
        BROWSER_PROFILE "/SingletonSocket",
        BROWSER_PROFILE "/SingletonCookie",
    };
    struct stat st;
    if (lstat(singletons[0], &st) != 0 || browser_is_running()) {
        return;
    }
    for (size_t i = 0; i < sizeof(singletons) / sizeof(singletons[0]); i++) {
        unlink(singletons[i]);
    }
    fprintf(stderr, "browser: removed the profile lock a browser that did not exit left behind\n");
}

/* Extra switches, one per line, from /etc/chromium-flags.conf - the file
   Arch Linux's chromium launcher reads for the same reason: a switch a
   person needs (a --v=1 to see why something failed, a feature turned off)
   should not mean rebuilding the program that passes switches. Lines
   starting with # are comments. */
static char flags_text[2048];

static int read_flags(char **out, int room) {
    FILE *file = fopen(BROWSER_FLAGS_FILE, "r");
    if (!file) {
        return 0;
    }
    size_t length = fread(flags_text, 1, sizeof(flags_text) - 1, file);
    fclose(file);
    flags_text[length] = 0;
    int count = 0;
    char *p = flags_text;
    while (*p && count < room) {
        char *line = p;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        char *end = line + strlen(line);
        while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
            *--end = 0;
        }
        if (*line && *line != '#') {
            out[count++] = line;
        }
    }
    return count;
}

int main(int argc, char **argv) {
    const char *page = argc > 1 && argv[1][0] ? argv[1] : BROWSER_HOME_PAGE;
    /* Chromium's logging writes to descriptor 2 and this libc's stderr is
       descriptor 1, because a program spawned from the desktop has a
       console on 1 and nothing on 2. Every LOG line the browser writes -
       including the one that says why it is dying - would otherwise go to a
       descriptor that is not there. The boot battery's harness does the
       same dup2 onto its pipe (kernel.c, [m167] and [m169]). */
    dup2(1, 2);
    /* fontconfig writes its cache under /tmp/fontconfig and says "No writable
       cache directories" when nothing has made it, then rescans every font on
       every start. Making it is the launcher's job because /tmp is a fresh
       directory on every boot. */
    mkdir("/tmp/fontconfig", 0700);

    /* Chromium's own browser - //chrome, the one with tabs, an omnibox,
       settings and a profile - is what the desktop opens when it is on the
       image (M187). content_shell is the embedder the Chromium tree ships to
       test //content with, and it stays the fallback on an image built
       before that, because an image without the full browser is still a
       valid image.

       A profile lives somewhere, and Chromium finds it through $HOME, which
       a program spawned from this desktop does not have. This machine has
       one principal and /home is theirs, so that is HOME and the profile
       is named under it rather than left to a fallback in /tmp - which is
       a fresh directory on every boot and would forget every tab. */
    /* The window is sized for the desktop it opens on. 900x640 is what the
       input suite's coordinates are measured against on QEMU's 1024x768; on
       a laptop's 1920x1200 desktop (M196's scaled 4K panel) the same window
       is a postcard in the corner. */
    static char window_size[48];
    {
        window_manager_framebuffer_info_t screen;
        int32_t width = 900, height = 640;
        if (sys_framebuffer_info(&screen) == 0 && screen.width >= 1600 && screen.height >= 1000) {
            width = (int32_t)screen.width - 320;
            height = (int32_t)screen.height - 240;
            if (width > 1600) {
                width = 1600;
            }
            if (height > 1000) {
                height = 1000;
            }
        }
        snprintf(window_size, sizeof(window_size), "--window-size=%d,%d", (int)width, (int)height);
    }

    if (exists(BROWSER_PROGRAM)) {
        setenv("HOME", PATH_HOME, 1);
        forget_stale_profile_lock();
        const char *const fixed[] = {
            BROWSER_PROGRAM,
            "--ozone-platform=leanos",
            "--disable-gpu",
            "--in-process-gpu",
            window_size,
            "--user-data-dir=" BROWSER_PROFILE,
            "--no-first-run",
            "--no-default-browser-check",
            /* One renderer process shared by every tab, which is Chromium's
               own switch for machines short of memory - and this one is,
               for a reason that is this kernel's: every process that execs
               /bin/chrome gets a private copy of its 306 MB image, so a
               browser, its services and three renderers do not fit in the
               4 GiB the harnesses give the machine, and the third tab's
               renderer failed to exec (M187). The condition for removing it
               is an executable's read-only pages shared by the processes
               running it. */
            "--renderer-process-limit=1",
            "--enable-logging=stderr",
            "--v=0",
        };
        char *exec_argv[sizeof(fixed) / sizeof(fixed[0]) + BROWSER_MAX_FLAGS + 2];
        int argc_out = 0;
        for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
            exec_argv[argc_out++] = (char *)fixed[i];
        }
        int extra = read_flags(exec_argv + argc_out, BROWSER_MAX_FLAGS);
        argc_out += extra;
        exec_argv[argc_out++] = (char *)page;
        exec_argv[argc_out] = 0;
        fprintf(stderr, "browser: exec %s --ozone-platform=leanos %s", BROWSER_PROGRAM, page);
        if (extra) {
            fprintf(stderr, " (and %d switch(es) from " BROWSER_FLAGS_FILE ")", extra);
        }
        fprintf(stderr, "\n");
        execv(BROWSER_PROGRAM, exec_argv);
    }

    char *const shell_argv[] = {
        SHELL_PROGRAM,
        "--ozone-platform=leanos",
        "--disable-gpu",
        "--in-process-gpu",
        "--content-shell-host-window-size=900x640",
        "--enable-logging=stderr",
        "--v=0",
        (char *)page,
        0,
    };
    fprintf(stderr, "browser: exec %s --ozone-platform=leanos %s\n",
            SHELL_PROGRAM, page);
    execv(SHELL_PROGRAM, shell_argv);
    const char *message = "browser: neither /bin/chrome nor /bin/chromiumshell "
                          "is on this image - `make browser` installs them\n";
    sys_write(2, message, strlen(message));
    return 1;
}
