/* user_space/bin/captest.c
 *
 * M65's self-test, in user space for the third time and the same reason
 * as badptr.c (M52), libctest.c (M63) and nettest.c (M64): what is new
 * is a *boundary at the syscall*, and a test that ran inside the kernel
 * would be on the wrong side of it.
 *
 * This program is granted nothing beyond CAP_APP_DEFAULT by the manifest
 * in system_api/include/caps.h, and that is the point - almost every
 * assertion here is that something it tried was refused. An ordinary
 * desktop application is what this program is pretending to be, and the
 * claim is that an ordinary desktop application cannot paint on the
 * screen, cannot read the clipboard, cannot enumerate processes, cannot
 * open a socket, cannot change the resolution, cannot set the clock and
 * cannot switch the machine off.
 *
 * The two things it *can* do are checked too, because a capability model
 * that refuses everything is indistinguishable from a broken kernel.
 *
 * Exit 0 for all-passed, 1 otherwise.
 */
#include <stdio.h>
#include <string.h>

#include "caps.h"
#include "os_net.h"
#include "power_mode.h"
#include "signal.h"
#include "proc.h"
#include "syscall_wrappers.h"
#include "wm.h"

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("captest: FAILED - %s\n", what);
        failures++;
    }
}

int main(void) {
    uint32_t mine = (uint32_t)sys_getcaps();

    /* ---- the manifest was applied at all ------------------------------ */
    check(mine == CAP_APP_DEFAULT,
          "this program did not start with the default application capability set");

    /* ---- what an application may not do -------------------------------- */
    check((uint64_t)sys_fb_map() == (uint64_t)-1, "an ordinary program mapped the framebuffer");

    char clip[16];
    check(sys_clipboard_get(clip, sizeof(clip)) < 0, "an ordinary program read the clipboard");
    check(sys_clipboard_set("stolen", 6) < 0, "an ordinary program wrote the clipboard");

    static task_info_t infos[TASK_INFO_MAX];
    check(sys_taskinfo(infos, TASK_INFO_MAX) < 0, "an ordinary program listed every process");

    check(sys_socket(OS_SOCK_DGRAM) < 0, "an ordinary program opened a socket");

    check(sys_display_set_mode(800, 600) < 0, "an ordinary program changed the screen resolution");

    check(sys_settime(1800000000u) < 0, "an ordinary program set the system clock");

    check(sys_audio_claim() < 0, "an ordinary program claimed the sound hardware");

    /* Not tested by *calling* it: a successful SYS_shutdown does not
     * return, so an unrefused one would end the machine mid-self-test
     * and leave no evidence of why. The check is that the capability is
     * absent, which is the same claim the gate makes. */
    check((mine & CAP_POWER) == 0, "an ordinary program holds the power capability");

    /* ---- who you may signal -------------------------------------------- */
    /* PID 1 is init, which is emphatically not one of this program's
     * children. Without CAP_KILL_ANY this must be refused - and the
     * consequence of getting it wrong is not subtle, which is why this
     * is the assertion worth having: a kernel that let this through
     * would have this test take the whole desktop down with it, and the
     * boot self-test checks that init is still running afterwards. */
    check(sys_kill(1, SIGKILL) < 0, "an ordinary program killed init");

    /* But a parent may always end what it started, capability or not -
     * the relationship that gave you the pid is what entitles you to use
     * it, and a launcher that cannot stop its own children is not a
     * launcher. */
    long child = sys_spawn("/bin/hello", 0);
    if (child > 0) {
        check(sys_kill((int)child, SIGKILL) == 0, "a program could not kill its own child");
        sys_wait((int)child);
    } else {
        check(0, "could not spawn a child to test signalling one");
    }

    /* ---- what it may do ------------------------------------------------ */
    /* Geometry is not authority: every program is entitled to know how
     * big the screen is, and gating that would break layout in programs
     * that never touch a pixel of it. */
    wm_fb_info_t fb;
    check(sys_fb_info(&fb) == 0, "an ordinary program could not ask the screen's size");
    check(fb.width > 0 && fb.height > 0, "the screen geometry came back empty");

    /* And it can use the filesystem, which is nearly the whole of what
     * CAP_APP_DEFAULT is. */
    const char *path = "/tmp/captest.txt";
    check(sys_writefile(path, "kept", 4) == 0, "an ordinary program could not write a file");
    char back[8];
    memset(back, 0, sizeof(back));
    check(sys_readfile(path, back, sizeof(back)) == 4, "the file did not read back");
    check(memcmp(back, "kept", 4) == 0, "the file read back wrong");
    check(sys_unlink(path) == 0, "an ordinary program could not delete its own file");

    /* ---- dropping is one-way ------------------------------------------- */
    long after = sys_dropcaps(0);
    check(after == 0, "sys_dropcaps(0) did not clear every capability");
    check(sys_getcaps() == 0, "capabilities came back after being dropped");
    /* The whole model rests on this: nothing anywhere can put a bit
     * back. sys_dropcaps is the only call that touches the mask and it
     * only ever ANDs, so asking for everything gets nothing. */
    check(sys_dropcaps(CAP_ALL) == 0, "sys_dropcaps handed capabilities back");
    check(sys_writefile("/tmp/captest2.txt", "no", 2) < 0,
          "a program that dropped CAP_FS_WRITE could still write a file");

    if (failures) {
        printf("captest: %d check(s) failed\n", failures);
        return 1;
    }
    printf("captest: all checks passed\n");
    return 0;
}
