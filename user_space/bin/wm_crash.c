/* user_space/bin/wm_crash.c
 *
 * M55: kills the compositor, on purpose, from user space.
 *
 * Same self-test-only role wm_demo.c has held since M20 and wm_faulter.c
 * since M52. It exists because M55's claim - that losing the compositor
 * is a flicker rather than the end of the session - is only worth
 * anything if it can be triggered the way a person would trigger it, and
 * there was no such way. The task manager refuses the desktop's own
 * processes (task_manager.c's PROTECTED, and rightly: three of the four
 * entries on that list are things you would only ever kill by mistake),
 * and nothing else in this OS can name a process at all.
 *
 * So this is the smallest program that can: enumerate the task table,
 * find the one called "compositor", and SIGKILL it. No arguments, no
 * confirmation, no output beyond a line saying what it did - it is a test
 * fixture, and anything more would be pretending it is a tool.
 *
 * It deliberately kills by *name* rather than taking a pid, because the
 * thing being tested is reachable from a launcher that can only start
 * programs, not pass them arguments a person would have to look up first.
 */
#include "proc.h" /* system_api/include/proc.h - task_info_t, TASK_INFO_MAX */
#include "signal.h"
#include "str.h"
#include "syscall_wrappers.h"

static void put(const char *s) {
    sys_write(1, s, strlen(s));
}

int main(void) {
    static task_info_t infos[TASK_INFO_MAX];
    long n = sys_taskinfo(infos, TASK_INFO_MAX);
    if (n <= 0) {
        put("wm_crash: SYS_taskinfo returned nothing\n");
        return 1;
    }
    for (long i = 0; i < n; i++) {
        if (infos[i].state != TASK_INFO_TERMINATED && strcmp(infos[i].name, "compositor") == 0) {
            put("wm_crash: killing the compositor\n");
            sys_kill(infos[i].pid, SIGKILL);
            return 0;
        }
    }
    put("wm_crash: no compositor is running\n");
    return 1;
}
