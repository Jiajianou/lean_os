#include "proc.h"
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
