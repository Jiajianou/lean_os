#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "paths.h"
#include "syscall_wrappers.h"

/* The desktop's Browser: Chromium's own content_shell, started the way this
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

   --single-process is a measurement rather than a preference, recorded in
   M169: a renderer in a process of its own does not yet submit a compositor
   frame on this machine. When it does, this line goes and the renderer's
   capability sandbox (M166) is what a page runs under. */

#define BROWSER_PROGRAM PATH_BIN_DIRECTORY "chromiumshell"
#define BROWSER_HOME_PAGE "file:///usr/share/browser/home.html"

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
    char *const exec_argv[] = {
        BROWSER_PROGRAM,
        "--ozone-platform=leanos",
        "--disable-gpu",
        "--single-process",
        "--content-shell-host-window-size=900x640",
        "--enable-logging=stderr",
        "--v=0",
        (char *)page,
        0,
    };
    fprintf(stderr, "browser: exec %s --ozone-platform=leanos %s\n", BROWSER_PROGRAM, page);
    execv(BROWSER_PROGRAM, exec_argv);
    const char *message = "browser: /bin/chromiumshell is not on this image - "
                          "`make browser` builds and installs it\n";
    sys_write(2, message, strlen(message));
    return 1;
}
