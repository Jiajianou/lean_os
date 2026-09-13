#include <stdio.h>
#include <string.h>

#include "capabilities.h"
#include "os_network.h"
#include "power_mode.h"
#include "signal.h"
#include "process.h"
#include "syscall_wrappers.h"
#include "window_manager.h"

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("captest: FAILED - %s\n", what);
        failures++;
    }
}

int main(int argc, char **argv) {
    uint32_t mine = (uint32_t)sys_getcaps();

    check(mine == CAP_APP_DEFAULT,
          "this program did not start with the default application capability set");

    check((uint64_t)sys_framebuffer_map() == (uint64_t)-1, "an ordinary program mapped the framebuffer");

    char clip[16];
    check(sys_clipboard_get(clip, sizeof(clip)) < 0, "an ordinary program read the clipboard");
    check(sys_clipboard_set("stolen", 6) < 0, "an ordinary program wrote the clipboard");

    static task_info_t infos[TASK_INFO_MAX];
    check(sys_taskinfo(infos, TASK_INFO_MAX) < 0, "an ordinary program listed every process");

    check(sys_socket(OS_SOCKET_DGRAM) < 0, "an ordinary program opened a socket");

    check(sys_display_set_mode(800, 600) < 0, "an ordinary program changed the screen resolution");

    check(sys_settime(1800000000u) < 0, "an ordinary program set the system clock");

    check(sys_audio_claim() < 0, "an ordinary program claimed the sound hardware");

    check((mine & CAP_POWER) == 0, "an ordinary program holds the power capability");

    int victim = 0;
    if (argc > 1) {
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            victim = victim * 10 + (*p - '0');
        }
    }
    if (victim > 0) {
        check(sys_kill(victim, SIGKILL) < 0,
              "an ordinary program killed a process that is not its child");
    } else {
        check(0, "no victim pid on the command line - nothing to prove the kill refusal against");
    }

    long child = sys_spawn("/bin/hello", 0);
    if (child > 0) {
        check(sys_kill((int)child, SIGKILL) == 0, "a program could not kill its own child");
        sys_wait((int)child);
    } else {
        check(0, "could not spawn a child to test signalling one");
    }

    window_manager_framebuffer_info_t framebuffer;
    check(sys_framebuffer_info(&framebuffer) == 0, "an ordinary program could not ask the screen's size");
    check(framebuffer.width > 0 && framebuffer.height > 0, "the screen geometry came back empty");

    const char *path = "/tmp/captest.txt";
    check(sys_writefile(path, "kept", 4) == 0, "an ordinary program could not write a file");
    char back[8];
    memset(back, 0, sizeof(back));
    check(sys_readfile(path, back, sizeof(back)) == 4, "the file did not read back");
    check(memcmp(back, "kept", 4) == 0, "the file read back wrong");
    check(sys_unlink(path) == 0, "an ordinary program could not delete its own file");

    long after = sys_dropcaps(0);
    check(after == 0, "sys_dropcaps(0) did not clear every capability");
    check(sys_getcaps() == 0, "capabilities came back after being dropped");
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
